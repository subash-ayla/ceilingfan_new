#
# Copyright 2022 Ayla Networks, Inc.  All rights reserved.
#
cmake_minimum_required(VERSION 3.5)

#
# Use make to invoke idf.py or set SDKCONFIG from the
# idf.py command line with -D "SDKCONFIG=<file>"
#
if(NOT DEFINED SDKCONFIG)
	message(FATAL_ERROR "SDKCONFIG not specified")
endif()
set(SDKCONFIG_DEFAULTS ${SDKCONFIG})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bootloader_pha)
