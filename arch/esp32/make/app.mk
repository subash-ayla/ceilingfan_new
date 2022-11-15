#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#

#
# General makefile for ESP32 Apps
#

#
# Input variables:
#
# TOP is the relative path to the top of the git tree
# DIR is the relative path to the application from the top of the git tree.
# APP is the name of the application to build
# APP_DEST is path to the app build directory relative to the ADA PKG directory
#

#
# IDF_PATH is relative to the where the make takes place (../$(APP))
#
IDF_PATH := $(TOP)/ada/bc/build/pkg/ada-esp-idf/esp-idf
export IDF_PATH

QUIET ?= @
NULL :=

#
# The default target (because it's first).
#
.PHONY: default
default: $(APP)

#
# List of directories to search files to check for style.
# These are relative to the $(APP) directory.
#
CSTYLE_DIRS += \
	main \
	$(NULL)

include $(TOP)/arch/esp32/make/cstyle.mk

#
# Targets in the $(ADA)/Makefile
# Most of these are passed to the app's Makefile in the SDK
# The clean target is not passed into the SDK, but cleans the entire build
#
ADA_TARGETS = \
	all \
	app \
	app-flash \
	app-clean \
	bootloader \
	bootloader-clean \
	bootloader-flash \
	clean \
	defconfig \
	erase_flash \
	erase_ota \
	flash \
	help \
	list-components \
	menuconfig \
	monitor \
	partition_table \
	print_flash_cmd \
	sdk \
	simple_monitor \
	size \
	size-components \
	size-files \
	size-symbols \
	$(NULL)

#
# Rule for the ADA_TARGETS listed above
#
.PHONY: $(ADA_TARGETS)
.PHONY: FORCE
$(ADA_TARGETS): FORCE
	$(QUIET)echo "\nBuilding ADA target: $@"; \
	$(MAKE) -C $(ADA) ESP32_APP=$(APP_DEST) $@

#
# Rule for the app
#
.PHONY: $(APP)
$(APP): cstyle sdk
