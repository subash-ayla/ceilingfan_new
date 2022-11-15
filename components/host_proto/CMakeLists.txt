#
# Copyright 2021 Ayla Networks, Inc.  All rights reserved.
#
set(SOURCES
		"conf_tlv.c"
		"data_tlv.c"
		"data_tlv_wifi.c"
		"host_decode.c"
		"host_prop.c"
		"host_proto.c"
		"host_proto_ota.c"
		"hp_buf.c"
		"hp_buf_cb.c"
		"hp_buf_tlv.c"
		"mcu_uart.c"
		"prop_req.c"
	)
set(CSTYLE_SOURCES
		"${SOURCES}"
		"conf_tlv.h"
		"data_tlv.h"
		"host_decode.h"
		"host_prop.h"
		"host_proto_int.h"
		"host_proto_ota.h"
		"hp_buf.h"
		"hp_buf_cb.h"
		"hp_buf_tlv.h"
		"include/host_proto/host_proto.h"
		"include/host_proto/mcu_dev.h"
		"mcu_uart_int.h"
		"prop_req.h"
	)

idf_component_register(SRCS "${SOURCES}"
	INCLUDE_DIRS
		"."
		"include"
	REQUIRES
		ayla
		libapp
	)

target_compile_options(${COMPONENT_LIB} PRIVATE
		-DAYLA_HOST_PROP_ACK_SUPPORT
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
include(${REPO_ROOT}/cmake/cstyle.cmake)
add_cstyle_checks("${COMPONENT_LIB}" "${CSTYLE_SOURCES}")
