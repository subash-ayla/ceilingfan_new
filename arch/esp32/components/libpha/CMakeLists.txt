set(SOURCES
		"ftm.c"
		"ftm_wifi.c"
		"secure.c"
		"secure_cli.c"
	)
set(CSTYLE_SOURCES
		"${SOURCES}"
		"ftm_int.h"
		"include/libpha/ftm.h"
		"include/libpha/secure.h"
		"secure_int.h"
	)

idf_component_register(SRCS "${SOURCES}"
	INCLUDE_DIRS
		"."
		"include"
	REQUIRES
		ayla
		libapp
		console
		efuse
		esp_wifi
		lwip
		nvs_flash
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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${REPO_ROOT}/cmake/version.cmake)
include(${REPO_ROOT}/cmake/cstyle.cmake)
add_cstyle_checks(${COMPONENT_LIB} "${CSTYLE_SOURCES}")
