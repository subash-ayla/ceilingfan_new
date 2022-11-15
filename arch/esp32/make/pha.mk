#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#

#
# Common Makefile for Productized Host Apps (production agents).
#
NULL :=
QUIET ?= @

#
# Set optional features as desired by uncommenting the relevant CFLAGS
# The including makefile can define some of these if not all PHAs need them.
#
# CFLAGS += -DAYLA_FILE_PROP_SUPPORT
# CFLAGS += -DAYLA_BATCH_PROP_SUPPORT
CFLAGS += -DAYLA_LAN_SUPPORT
CFLAGS += -DAYLA_BLUETOOTH_SUPPORT
CFLAGS += -DAYLA_METRICS_SUPPORT
CFLAGS += -DAYLA_PRODUCTION_AGENT
CFLAGS += -DAYLA_LOG_COREDUMP
CFLAGS += -DAYLA_LOG_SNAPSHOTS

#
# Settings for download target.
# Flash mode and frequency can use the settings in the app binary image (keep)
# as configured via sdkconfig in Flash Tool Settings.
# Otherwise, use qout and 80m for devices with the latest booter.
# Use dout and 40m for boards with older booters.
#
FLASH_MODE ?= keep
FLASH_FREQ ?= keep
ESPPORT ?= /dev/ttyUSB0
ESPBAUD ?= 921600
ESPTOOL = $(IDF_PATH)/components/esptool_py/esptool/esptool.py
ESPSECURE = $(IDF_PATH)/components/esptool_py/esptool/espsecure.py
SIGNING_KEY ?= $(TOP)/arch/esp32/conf/eng_key.pem
PART_SIZES ?= 4M 8M
MODULES ?=

#
# Secret portion of setup-mode re-enable key.
#
SETUP_EN_SECRET := "\"\\xfb\\x83\\xb7\\x9b\\x79\\x8d\\xeb\\x95\""

CFLAGS += -DLIBAPP_SETUP_EN_SECRET=$(SETUP_EN_SECRET)

#
# Include libapp
#
EXTRA_COMPONENT_DIRS += $(TOP)/arch/esp32/components/libapp
EXTRA_COMPONENT_DIRS += $(TOP)/arch/esp32/components/libpha
CSTYLE_DIRS += $(TOP)/arch/esp32/components/libpha

#
# Include Firedome
#
MAKE_SDK_INCLUDE_FIREDOME ?= y
ifeq ($(MAKE_SDK_INCLUDE_FIREDOME),y)
CFLAGS += -DAYLA_FIREDOME_SUPPORT
EXTRA_COMPONENT_DIRS += $(TOP)/ext/firedome/component/ayla_firedome
EXTRA_COMPONENT_DIRS += $(TOP)/ext/firedome/component/firedome
endif

#
# Build the App using common definitions
#
include $(TOP)/arch/esp32/make/sdk.mk

#
# Sign the app with SIGNING_KEY
#
build/$(APP)-signed.bin: build/$(APP).bin
	$(QUIET)echo signing with $(notdir $(SIGNING_KEY)); \
	python $(ESPSECURE) sign_data --keyfile $(SIGNING_KEY) --version 2 \
		--output $@ $<

.PHONY: sign
sign: build/$(APP)-signed.bin
	$(QUIET)echo signed image is $<

#
# Convenient download target
#
.PHONY: download
download: cstyle all download_only

.PHONY: download_only
download_only: build/$(APP)-signed.bin
	python $(ESPTOOL) --chip esp32 \
		--port $(ESPPORT) --baud $(ESPBAUD) \
		--before default_reset --after hard_reset \
		write_flash --encrypt \
		--flash_mode $(FLASH_MODE) --flash_freq $(FLASH_FREQ) \
		--flash_size detect \
		0x100000 build/$(APP)-signed.bin

#
# Targets for app_pkg - production package for applications
#
# The config files are in conf/*.txt and the name is used in the
# package name.
#
# If CONF_PREF is specified, it can be used to select a subset of configs
# to build.
#
CONF_PREF ?=
CONF ?= $(wildcard conf/$(CONF_PREF)*.txt)
CONFS ?= $(basename $(notdir $(CONF)))

#
# make app_pkg.
#
# Note: this doesn't build the app.  Do 'make' first.
#
# Note: this currently only works for one OEM at a time, although the config
# files may include multiple OEMs.
# Supply PROD_OEM_SECRET on the command line.
#
.PHONY: app_pkg
.PHONY: FORCE
app_pkg: FORCE
	$(QUIET)set -e; \
	if [ -z "$(CONFS)" ]; then \
		echo "No configuration files present or none selected." >&2; \
		exit 1; \
	fi; \
	for prod_name in $(CONFS); \
	do \
		echo Making $(APP)-$$prod_name; \
		for prod_size in ${PART_SIZES}; \
		do \
			if [ -z "$(MODULES)" ]; then \
				$(MAKE) -C $(TOP)/arch/esp32/prod app_pkg \
					APP=$(APP) \
					APP_PROD=$$prod_name \
					APP_CONF=conf/$$prod_name.txt \
					PART_SIZE=$$prod_size; \
			else \
				for module in $(MODULES); \
				do \
					$(MAKE) -C \
						$(TOP)/arch/esp32/prod app_pkg \
						APP=$(APP) \
						APP_PROD=$$prod_name \
						APP_CONF=conf/$$prod_name.txt \
						PART_SIZE=$$prod_size \
						MODULE=$$module; \
				done
			fi
		done
	done
