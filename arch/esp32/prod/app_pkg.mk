#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#
APP ?= starter_app
APP_PKG_VER ?= 1.0

# product name
APP_PROD ?=

PART_SIZE ?= 8M
APP_CONF ?= conf/prod_config.txt

define HELP_TEXT
Build archive for Productized Host Application package.

Targets:
	app_pkg:	application package for APP

options, with defaults shown:
	APP=$(APP)		# The application to use, already built
	PART_SIZE=$(PART_SIZE)		# Flash size
	APP_PKG_VER=$(APP_PKG_VER)
	APP_PROD=$(APP_PROD)	# suffix for app: product name
	PROD_OEM_SECRET=	# OEM secret for prod_config.txt, required
	APP_CONF=$(APP_CONF)	# path to product config file from app dir
	SIGNING_KEY=$(SIGNING_KEY)	# private key .pem file to sign boot and app
endef

#
# relative directory names
# TOP = top of the workspace tree
#
TOP ?= ../../..
DIR ?= arch/esp32/prod
NULL :=
QUIET ?= @
APP ?= starter_app
APP_DIR ?= ../apps/$(APP)/build
LIB = pyayla
CONF_DIR = ../conf
PART_CSV = $(CONF_DIR)/flash_partitions_$(PART_SIZE).csv
BOOTLOADER ?= ../prebuilt/bootloader-sec-$(PART_SIZE).bin

GEN_ESPPART = $(IDF_PATH)/components/partition_table/gen_esp32part.py
NVS_PARTGEN_DIR = components/nvs_flash/nvs_partition_generator
ESPSECURE = $(IDF_PATH)/components/esptool_py/esptool/espsecure.py

#
# default target (because it's first)
#
.PHONY: default
default: app_pkg
	$(info For help, use 'make help')

.PHONY: help
help:
	@:
	$(info $(HELP_TEXT))

BUILD = $(TOP)/build

%.zip: %
	$(QUIET)echo Archiving $(notdir $@); \
	rm -f $@ && \
	cd $< && \
	zip -q -r ../$(notdir $@) . && \
	echo successful build result: $@

APP_PKG_BASE_NAME ?= \
	Ayla-$(APP)$(addprefix -,$(APP_PROD))-$(APP_PKG_VER)-$(PART_SIZE)
APP_PKG_BUILD = $(TOP)/build/$(APP_PKG_BASE_NAME)

#
# Files that become part of the app package.
# The order of these files should be kept alphabetical.
# It influences the md5 of the package
#
APP_PKG_FILES = \
	$(APP_PKG_BUILD)/images/app.bin \
	$(APP_PKG_BUILD)/images/bootloader.bin \
	$(APP_PKG_BUILD)/images/partition_table.bin \
	$(APP_PKG_BUILD)/prod_config.txt \
	$(NULL)

$(APP_PKG_BUILD):
	$(QUIET)mkdir -p $@ $@/images

#
# Use first 8 hex characters from SHA-256 hash to identify the package and the
# config file.
#
GEN_HASH = openssl sha256 -r | sed -e 's/\(........\).*/\1/'
APP_HASH = ${shell cat $(APP_PKG_FILES) | $(GEN_HASH)}
CONF_HASH = ${shell cat $(APP_PKG_BUILD)/prod_config.txt | $(GEN_HASH)}

APP_PKG_FULL_NAME = $(APP_PKG_BASE_NAME)-$(APP_HASH)-$(CONF_HASH)
APP_PKG = $(TOP)/build/$(APP_PKG_FULL_NAME)

#
# Final app package with generated name
#
.PHONY: app_pkg
app_pkg: $(APP_PKG_BUILD) $(APP_PKG_FILES)
	$(QUIET)echo making $(notdir $(APP_PKG)).zip; \
	rm -rf $(APP_PKG) $(APP_PKG).zip && \
	cp -R $(APP_PKG_BUILD) $(APP_PKG) && \
	(cd $(APP_PKG) && zip -q -r ../$(APP_PKG_FULL_NAME).zip .)

ifeq ($(SIGNING_KEY),)
$(error SIGNING_KEY=<key>.pem file parameter required)
endif

#
# App binary
#
APP_BIN := $(APP_DIR)/$(APP)-unsigned.bin
ifeq ("$(wildcard $(APP_BIN))","")
APP_BIN := $(APP_DIR)/$(APP).bin
endif
ifeq ("$(wildcard $(APP_BIN))","")
$(error make $(APP) first)
endif

$(APP_PKG_BUILD)/images/app.bin: $(APP_BIN)
	$(ESPSECURE) sign_data --version 2 --keyfile "$(SIGNING_KEY)" \
	    --output $@ $<

#
# Boot loader
#
$(APP_PKG_BUILD)/images/bootloader.bin: $(BOOTLOADER)
	$(ESPSECURE) sign_data --version 2 --keyfile "$(SIGNING_KEY)" \
	    --output $@ $<

#
# Partition table
#
$(APP_PKG_BUILD)/images/partition_table.bin: $(PART_CSV)
	$(GEN_ESPPART) --flash-size $(PART_SIZE)B $< $@

#
# Configuration file
#
ifeq ($(PROD_OEM_SECRET),)
$(APP_PKG_BUILD)/prod_config.txt: ../apps/$(APP)/$(APP_CONF)
	$(error PROD_OEM_SECRET must be in environment or on make command line)
else
$(APP_PKG_BUILD)/prod_config.txt: ../apps/$(APP)/$(APP_CONF)
	sed -e "s/--your-OEM-secret--/$(PROD_OEM_SECRET)/"  < $< > $@
endif
