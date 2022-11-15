/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */

/*
 * Support for factory test mode.
 */
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ayla/utypes.h>
#include <ayla/assert.h>
#include <ayla/log.h>
#include <ayla/mod_log.h>
#include <ada/libada.h>
#include <ada/ada_conf.h>
#include <libapp/libapp.h>
#include <libpha/ftm.h>
#include "ftm_int.h"
#include "secure_int.h"

static u8 pha_ftm_enabled;
static u8 pha_ftm_leave_enabled;	/* set if second AP is used */
static enum ftm_err pha_ftm_err;	/* saved error code for polling */

enum pha_ftm_state {
	FTMS_DEV = 0,		/* device tests running, as far as we know */
	FTMS_WIFI,		/* doing scan, signal, and connect tests */
	FTMS_BT,		/* bluetooth test */
	FTMS_SECURE,		/* enable boot security (may reboot) */
	FTMS_REENABLE,		/* using wifi to check for re-enable */
	FTMS_DONE,		/* all tests completed */
};
static enum pha_ftm_state pha_ftm_state;

void pha_ftm_log(const char *fmt, ...)
{
	ADA_VA_LIST args;

	ADA_VA_START(args, fmt);
	log_put_va(MOD_LOG_TEST, fmt, args);
	ADA_VA_END(args);
}

/*
 * Disable FTM after enabling boot security.
 *
 * If boot security is enabled successfully (not already on),
 * erase the enable config item from factory and startup config.
 * It should be in the startup configuration so a factory reset will remove it.
 * Leave other FTM config items present for possible re-enable.
 */
static void pha_ftm_disable(void)
{
	if (!pha_ftm_enabled) {
		return;
	}
	if (pha_ftm_leave_enabled) {
		pha_ftm_log(LOG_INFO "FTM: remains enabled");
		return;
	}

	/*
	 * Set flag causing lock attempts on future boots.
	 */
	libapp_conf_set(PHA_FTM_CONF_OK, "1", 1);

	/*
	 * Lock chip if possible then reboot to test new settings and
	 * to allow booter to enable security.
	 */
	if (!pha_ftm_secure_lock()) {
		pha_ftm_log(LOG_INFO "FTM resetting to apply boot security");
		esp_restart();
		ASSERT_NOTREACHED();
	}

	/*
	 * Erase FTM enable and OK flag.
	 */
	if (libapp_conf_set_blob(PHA_FTM_CONF_EN, NULL, 0, 1) < 0 ||
	    libapp_conf_set_blob(PHA_FTM_CONF_OK, NULL, 0, 1) < 0) {
		pha_ftm_log(LOG_ERR "FTM disable failed");
		return;
	}
	pha_ftm_log(LOG_INFO "FTM disabled");
}

/*
 * Handle factory reset.
 */
static void pha_ftm_factory_reset(void)
{
	u8 val = 0;

	if (pha_ftm_enabled) {
		libapp_conf_set_blob(PHA_FTM_CONF_OK, &val, sizeof(val), 1);
	}
}

/*
 * Return non-zero if factory test mode is enabled.
 * This has side-effects the first time it is called.
 */
int pha_ftm_is_enabled(void)
{
	static u8 get_done;
	char buf[10];
	int len;

	if (get_done) {
		return pha_ftm_enabled;
	}

	/*
	 * See if previously passed and disabled.
	 */
	len = libapp_conf_get_string(PHA_FTM_CONF_OK, buf, sizeof(buf));
	if (len > 0) {
		pha_ftm_log(LOG_DEBUG "FTM success read %s", buf);
		if (buf[0] != '1') {
			pha_ftm_err = FTM_ERR_FACT_RST;
		}
		pha_ftm_state = FTMS_SECURE;
		pha_ftm_enabled = 1;
		get_done = 1;
		libapp_conf_factory_reset_callback_set(pha_ftm_disable);
		return 1;
	}

	len = libapp_conf_get_string(PHA_FTM_CONF_EN, buf, sizeof(buf));
	if (len > 0) {
		pha_ftm_log(LOG_DEBUG "FTM enable read %s", buf);
	} else {
		pha_ftm_log(LOG_DEBUG "FTM enable read len %d", len);
	}
	get_done = 1;
	if (len > 0 && buf[0] == '1') {
		pha_ftm_enabled = 1;
		libapp_conf_factory_reset_callback_set(pha_ftm_factory_reset);
		pha_ftm_wifi_poll(); /* start scan for connection test */
#ifdef AYLA_BLUETOOTH_SUPPORT
		libapp_bt_ftm_enable();
#endif
	} else {
		pha_ftm_wifi_restart_init();
	}
	return pha_ftm_enabled;
}

void pha_ftm_leave_enabled_set(void)
{
	pha_ftm_leave_enabled = 1;
}

/*
 * Enable FTM and restart
 * Do a factory reset to clean up any state left behind by the join.
 */
void pha_ftm_enable_restart(void)
{
	libapp_conf_set(PHA_FTM_CONF_EN, "1", 1);
	libapp_conf_set(PHA_FTM_CONF_OK, "", 1);
	pha_ftm_log(LOG_WARN "restarting with factory test mode enabled");
	ada_conf_reset(1);
}

/*
 * Run rest of factory test mode tests.
 * Called after device-specific tests are complete.
 * This version runs synchronously.  It should be replaced by pha_ftm_poll().
 */
enum ftm_err pha_ftm_run(void)
{
	enum ftm_err err;

	pha_ftm_log(LOG_DEBUG "pha_ftm_run starting");
	for (;;) {
		err = pha_ftm_poll();
		if (err != FTM_ERR_IN_PROGRESS) {
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(FTM_POLL_MS));
	}
	pha_ftm_log(LOG_DEBUG "pha_ftm_run returns %u", err);
	return err;
}

/*
 * Run next step of factory mode tests.
 * This does not block and should be called periodically every FTM_POLL_MS ms.
 * Returns FTM_ERR_IN_PROGRESS if not complete.
 * Returns 0 on success, and error code otherwise.
 */
enum ftm_err pha_ftm_poll(void)
{
	enum ftm_err err = FTM_ERR_NONE;
	static enum pha_ftm_state prev_state;

	if (pha_ftm_state != prev_state) {
		prev_state = pha_ftm_state;
		pha_ftm_log(LOG_DEBUG "FTM poll state %u",
		    pha_ftm_state);
	}
	switch (pha_ftm_state) {
	case FTMS_DEV:
		pha_ftm_state = FTMS_WIFI;
		/* fall-through */
	case FTMS_WIFI:
		err = pha_ftm_wifi_poll();
		if (err) {
			break;
		}
		pha_ftm_state = FTMS_BT;
		/* fall-through */
	case FTMS_BT:
		/* to be implemented later */
		pha_ftm_state = FTMS_SECURE;
		/* fall through */
	case FTMS_SECURE:
		pha_ftm_disable();
		pha_ftm_state = FTMS_DONE;
		/* fall through */
	case FTMS_DONE:
	default:
		err = pha_ftm_err;
		break;
	}
	if (err && err != FTM_ERR_IN_PROGRESS) {
		pha_ftm_err = err;
		pha_ftm_state = FTMS_DONE;
	}
	return err;
}

enum ftm_err pha_ftm_enable_security(void)
{
	libapp_conf_set(PHA_FTM_CONF_OK, "1", 1);
	(void)pha_ftm_is_enabled();
	pha_ftm_state = FTMS_SECURE;
	return pha_ftm_run();
}
