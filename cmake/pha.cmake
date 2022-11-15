#
# Copyright 2022 Ayla Networks, Inc.  All rights reserved.
#

#
# Common definitions for fast-track apps using libpha
#

idf_build_set_property(COMPILE_OPTIONS "-DAYLA_BLUETOOTH_SUPPORT" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DAYLA_LAN_SUPPORT" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DAYLA_LOG_CORE" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DAYLA_LOG_SNAPSHOTS" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DAYLA_METRICS_SUPPORT" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DAYLA_PRODUCTION_AGENT" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DLIBAPP_OTA_MODULE" APPEND)

set(SETUP_EN_SECRET "\"\\xfb\\x83\\xb7\\x9b\\x79\\x8d\\xeb\\x95\"")
idf_build_set_property(COMPILE_OPTIONS
	 "-DLIBAPP_SETUP_EN_SECRET=${SETUP_EN_SECRET}"
	 APPEND)

include(${REPO_ROOT}/cmake/ada_check.cmake)
