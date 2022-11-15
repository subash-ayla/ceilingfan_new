/*
 * Copyright 2020 Ayla Networks, Inc.  All rights reserved.
 */

#ifdef AYLA_BLUETOOTH_SUPPORT

#include <stddef.h>
#include <string.h>

#include <ayla/log.h>
#include <ayla/mod_log.h>
#include <sys/types.h>
#include <ayla/random.h>
#include <ada/libada.h>
#include <ada/generic_session.h>
#include <adw/wifi.h>
#include <adb/adb.h>
#include <adb/al_bt.h>
#include <adb/adb_ayla_svc.h>
#include <adb/adb_conn_svc.h>
#include <adb/adb_wifi_cfg_svc.h>
#include <adb/adb_mbox_svc.h>

#include <libapp/libapp.h>
#include <libapp/net_event.h>

static u8 bt_factory_test_mode;	/* disable advertisements during factory test */

/*
 * Set callback to be called when passkey needs to be displayed
 */
void libapp_bt_passkey_callback_set(void (*callback)(u32 passkey))
{
	al_bt_passkey_callback_set(callback);
}

/*
 * Enable factory test mode for BLE.
 * This must be called before libapp_bt_init().
 * For now, this disables BLE completely.
 */
void libapp_bt_ftm_enable(void)
{
	bt_factory_test_mode = 1;
}

void libapp_bt_adb_event_handler(enum adb_event event)
{
	switch (event) {
	case ADB_EV_WIFI_PROVISION_START:
		libapp_net_event_send(NS_PROVISIONING);
		break;
	case ADB_EV_WIFI_PROVISION_STOP:
		libapp_net_event_send(NS_PROVISIONING_STOPPED);
		break;
	default:
		break;
	}
}

void libapp_bt_register_services_cb(void)
{
	adb_ayla_svc_identify_cb_set(libapp_identify);
	adb_ayla_svc_register(NULL);
	adb_conn_svc_register(NULL);
#if defined(AYLA_LOCAL_CONTROL_SUPPORT) || defined(AYLA_TEST_SERVICE_SUPPORT)
	adb_mbox_svc_register(NULL);
#endif
	adb_wifi_cfg_svc_register(NULL);
}

/*
 * Initialize and start the Bluetooth demo
 */
void libapp_bt_init(void)
{
	if (bt_factory_test_mode) {
		al_bt_deinit();
		return;
	}

	adb_event_register(libapp_bt_adb_event_handler);
	al_bt_init(libapp_bt_register_services_cb);
	libapp_conf_factory_reset_callback_set(al_bt_conf_factory_reset);
}
#endif
