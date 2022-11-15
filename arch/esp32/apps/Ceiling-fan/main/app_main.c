/*
 * Copyright 2021 Ayla Networks, Inc.  All rights reserved.
 */

/*
 * This file has the initialization.
 */
#include <stdbool.h>
#include <string.h>
#include <ada/libada.h>
#include <ada/sprop.h>
#include <libapp/libapp.h>
#include <libapp/power_mgmt.h>
/*#include <ayla_firedome/libafd.h>*/
#include "app_build.h"
#include "app_int.h"
#include "app_logic.h"
#include <ayla/log.h>

#define BUILD_STRING APP_VER BUILD_SUFFIX " " \
	BUILD_DATE " " BUILD_TIME " " BUILD_VERSION

/*
 * The template version should select the template that matches the product.
 * It should not change unless properties are added or removed firmware.
 * This should be simple, like "1.0".
 */
#define TEMPLATE_VERSION "V1.0"

/*
 * Module software version and build string to be reported by the agent.
 */
const char mod_sw_version[] = APP_NAME " " BUILD_STRING;

char template_version[] = TEMPLATE_VERSION;

/*
 * Value for version property, reported as the host software version.
 */
static char version[] = APP_NAME " " BUILD_STRING;

static struct ada_sprop app_props[] = {
	/*
	 * version properties
	 * oem_host_version is the template version and must be sent first.
	 */
	{ "oem_host_version", ATLV_UTF8,
	    template_version, sizeof(template_version),
		ada_sprop_get_string, NULL},
	{ "version", ATLV_UTF8, &version[0], sizeof(version),
	    ada_sprop_get_string, NULL},
};

#ifdef AYLA_BLUETOOTH_SUPPORT
/*
 * Callback when the device should identify itself to the end user, for
 * example, briefly blinking an LED.
 */
void demo_identify_cb(void)
{
	printf("%s called\n", __func__);
}
#endif

/*
 * Initialize GPIOs and property managers.
 */
static void app_init(void)
{
	ada_sprop_mgr_register("app", app_props, ARRAY_LEN(app_props));

	/*
	 * Initialize AC protocol and get model and capabilities
	 */
	app_logic_init();

	/* Initialize application tasks, timers, properties, etc */
	app_logic_prop_init();

	libapp_sched_init(APP_SCHED_FORMAT, APP_SCHED_COUNT);
	/*libafd_firedome_init(NULL);*/
}

/*
 * Called when agent is first connected to the cloud.
 */
static void app_first_connect(void)
{
	log_put(LOG_INFO "%s: connected with cloud", __func__);

	ada_sprop_send_by_name("oem_host_version");
	ada_sprop_send_by_name("version");

	/* Update local status to cloud */
	ada_sprop_send_by_name("mode");
	ada_sprop_send_by_name("power");

	/* Send parts from-device props to cloud */
	ada_sprop_send_by_name("filter_installed_date");
	ada_sprop_send_by_name("filter_hours_used");
	ada_sprop_send_by_name("filter_status");
	ada_sprop_send_by_name("filter_max_life");
}

/*
 * Main entry point.
 */
void app_main(void)
{
	libapp_power_mgmt_init();
	libapp_power_mgmt_acquire();

	/*
	 * Set model part number.
	 */
	snprintf(conf_sys_model, CONF_MODEL_MAX, APP_MODULE_PN);

	/*
	 * Initialize common app features.
	 */
	libapp_init();

	/*
	 * Start common app features.
	 * app_init() will be called after agent is initialized.
	 */
	libapp_start(app_init, app_first_connect);

	app_logic_idle();	/* Should not return */
}
