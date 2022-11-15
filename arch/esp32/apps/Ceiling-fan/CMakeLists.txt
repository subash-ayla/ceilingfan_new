#
# Copyright 2021 Ayla Networks, Inc.  All rights reserved.
#
cmake_minimum_required(VERSION 3.5)

set(APP_NAME Ceiling_Fan_app)
set(APP_VER 1.1.1)
#set(ADA_MIN_VERSION 1.11)
#set(ADA_REQUIRED_REV "df84ae5fd")
set(ADA_REQUIRED_REV "e3f51f06c")
get_filename_component(REPO_ROOT ../../../.. ABSOLUTE)

set(EXTRA_COMPONENT_DIRS "../../components"
#		"../../../../ext/firedome/component"
	)

include($ENV{IDF_PATH}/tools/cmake/ayla_project.cmake)
project(${APP_NAME})

#idf_build_set_property(COMPILE_OPTIONS "-DAPP_MODULE_ESP_MINI_1" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DAYLA_BLUETOOTH_SUPPORT" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DAYLA_WIFI_SUPPORT" APPEND)
# idf_build_set_property(COMPILE_OPTIONS "-DAYLA_FIREDOME_SUPPORT" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DAYLA_LAN_SUPPORT" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DAYLA_LOG_COREDUMP" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DAYLA_LOG_SNAPSHOTS" APPEND)
# idf_build_set_property(COMPILE_OPTIONS "-DAYLA_LOCAL_CONTROL_SUPPORT" APPEND)
# idf_build_set_property(COMPILE_OPTIONS "-DAYLA_FILE_PROP_SUPPORT" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DAYLA_PRODUCTION_AGENT" APPEND)

#
# For productized-host apps, define LIBAPP_OTA_MODULE to use only module OTAs.
# Leave this commented out during development to use only host OTAs.
#
idf_build_set_property(COMPILE_OPTIONS "-DLIBAPP_OTA_MODULE" APPEND)
