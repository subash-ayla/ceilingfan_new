get_filename_component(REPO_ROOT ../../../.. ABSOLUTE)
set(SOURCES
		"libapp_bt.c"
		"libapp_cli.c"
		"libapp_conf_load.c"
		"libapp_conf.c"
		"libapp_debounce.c"
		"libapp_diag.c"
		"libapp_net_event.c"
		"libapp_nvs.c"
		"libapp_ota.c"
		"libapp_sched.c"
		"libapp_start.c"
		"libapp_wifi.c"
		"power_mgmt.c"
	)
set(CSTYLE_SOURCES
		"${SOURCES}"
		"libapp_conf_int.h"
		"libapp_sched.h"
		"libapp_nvs_int.h"
		"libapp_conf_wifi.h"
		"libapp_command_table.h"
		"include/libapp/libapp_ota.h"
		"include/libapp/libapp.h"
		"include/libapp/net_event.h"
		"include/libapp/power_mgmt.h"
		"include/libapp/debounce.h"
	)

idf_component_register(SRCS
		"${SOURCES}"
	INCLUDE_DIRS
		"."
		"include"
	REQUIRES
		ayla
		console
		esp_wifi
		lwip
		nvs_flash
		${FIREDOME_COMPONENT}
	)

target_compile_options(${COMPONENT_LIB} PRIVATE
		-DAYLA_WIFI_SUPPORT
		-DAYLA_LOG_SNAPSHOTS
		-DAYLA_METRICS_SUPPORT
		-DAYLA_LOG_COREDUMP
		-DCONF_NO_ID_FILE
		-DLOG_LOCK
		-DAYLA_ESP32_SUPPORT
		-DLOG_SEV_SHORT
		-DLWIP_1_5_0_SUPPORT
		-D_HAS_ASSERT_F_
		-Wno-missing-field-initializers
	)

set(ADA_MIN_VERSION 1.9)
include(${REPO_ROOT}/cmake/ada_check.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${REPO_ROOT}/cmake/cstyle.cmake)
add_cstyle_checks(${COMPONENT_LIB} "${CSTYLE_SOURCES}")
