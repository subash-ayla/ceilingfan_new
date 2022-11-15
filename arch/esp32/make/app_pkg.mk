#
# Copyright 2021 Ayla Networks, Inc.  All rights reserved.
#

#
# Make application package for productized-host / fast-track app
#

APP_TARGET ?= esp32
PART_SIZE ?= 4M
ifeq ($(APP_TARGET),esp32c3)
PARTITION_VER ?= v2
else
PARTITION_VER ?= v1
endif
BUILD_TYPE ?= REL
BUILD_NAME ?=

#
# The bootloader will be signed by default.
# So far, only the esp32c3 handles multiple keys for the bootloader
#
SIGN_BOOT ?= y
ifeq ($(APP_TARGET),esp32c3)
SIGNING_KEY2 ?=
SIGNING_KEY3 ?=
else
SIGNING_KEY2 :=
SIGNING_KEY3 :=
endif

define HELP_TEXT
Build release package for production

Targets:
	app_pkg:	application package for APP

options, with defaults shown:
	APP=			# The application to use, already built
	APP_PKG_VER=		# application version
	APP_CONF=		# config file under app's conf subdirectory
	PART_SIZE=$(PART_SIZE)		# Flash size
	APP_PROD=$(APP_PROD)	# suffix for app: product name
	BOOTLOADER=$(BOOTLOADER) # alternative prebuilt bootloader
	BOOTLOADER_DEV=$(BOOTLOADER_DEV) # alternative development bootloader
	BOOTLOADER_SELECT=	# optional selection for bootloader
	BUILD_TYPE=$(BUILD_TYPE)	# REL or DEV
	BUILD_NAME=$(BUILD_NAME)	# e.g., "eng" or "beta"
	SIGNING_KEY=$(SIGNING_KEY) # private key .pem file to sign app
	SIGN_BOOT=$(SIGN_BOOT)		# if y, sign or re-sign bootloader
	SIGNING_KEY2=$(SIGNING_KEY2) # optional 2nd key to sign booter
	SIGNING_KEY3=$(SIGNING_KEY2) # optional 3rd key to sign booter
	SIGNING_KEY3_DEV=$(SIGNING_KEY2) # optional 3rd key to sign dev booter
	PARTITION_VER=$(PARTITION_VER)	# partition table version
endef

#
# relative directory names
# TOP = top of the workspace tree
#
TOP ?= ../../../..
NULL :=
QUIET ?= @
APP_DIR ?= .
APP_CONF_DIR = conf
CONF_DIR = ../../conf
ifneq ($(PARTITION_VER),v1)
PART_CSV ?= $(CONF_DIR)/flash_partitions_$(PARTITION_VER)_$(PART_SIZE).csv
else
PART_CSV ?= $(CONF_DIR)/flash_partitions_$(PART_SIZE).csv
endif
BOOT_DIR = $(TOP)/arch/$(APP_TARGET)/prebuilt
BOOTLOADER ?= $(BOOT_DIR)/bootloader-sec-$(PART_SIZE).bin
BOOTLOADER_SELECT ?= bootloader

GEN_ESPPART = $(IDF_PATH)/components/partition_table/gen_esp32part.py
NVS_PARTGEN_DIR = components/nvs_flash/nvs_partition_generator
ESPSECURE = $(IDF_PATH)/components/esptool_py/esptool/espsecure.py
IDF_PY = $(IDF_PATH)/tools/idf.py

#
# default target (because it's first)
#
.PHONY: app_pkg_default
app_pkg_default: app_pkg
	$(info For help, use 'make help')

.PHONY: help
help:
	@:
	$(info $(HELP_TEXT))

BUILD = $(TOP)/build
APP_BUILD = $(APP_DIR)/build/$(APP_TARGET)

APP_PKG_BASE_NAME ?= \
	Ayla-$(APP)$(addprefix -,$(APP_PROD))-$(APP_PKG_VER)-$(PART_SIZE)
APP_PKG_BUILD = $(BUILD)/$(APP_PKG_BASE_NAME)

#
# Support package with app files needed, same in all app packages using
# the same application and target. The rest is just configuation.
# The app image and bootloader will have been signed.
# This package should be kept for support purposes as it contains the ELF file.
#
APP_STAGING ?= \
	$(BUILD)/Ayla-$(APP)-$(APP_PKG_VER)-$(APP_TARGET)-$(PART_SIZE)-support

APP_STAGING_FILES = \
	$(APP_STAGING)/app.bin \
	$(APP_STAGING)/app.elf \
	$(APP_STAGING)/bootloader.bin \
	$(APP_STAGING)/partition_table.bin \
	$(NULL)

ifneq ($(BOOTLOADER_DEV),)
APP_STAGING_FILES += $(APP_STAGING)/bootloader-dev.bin
endif

#
# General rule to make a zip archive
#
%.zip: %
	$(QUIET)echo Archiving $(notdir $@); \
	rm -f $@ && \
	cd $< && \
	zip -q -r ../$(notdir $@) . && \
	echo successful build result: $@

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

$(APP_PKG_BUILD): FORCE
	$(QUIET)rm -rf $@
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
app_pkg: staging app_pkg_only

.PHONY: staging
staging: $(APP_STAGING).zip

#
# Target for app package from APP_STAGING without rebuilding the files.
#
.PHONY: app_pkg_only
app_pkg_only: $(APP_PKG_BUILD) $(APP_PKG_FILES)
	$(QUIET)echo making $(notdir $(APP_PKG)).zip; \
	rm -rf $(APP_PKG) $(APP_PKG).zip && \
	cp -R $(APP_PKG_BUILD) $(APP_PKG) && \
	(cd $(APP_PKG) && zip -q -r ../$(APP_PKG_FULL_NAME).zip .)

#
# App binary
# The wildcard expansion of file name will be empty if it doesn't exist.
# Use -unsigned.bin if it is present, otherwise the build didn't sign the image.
#
APP_BIN := $(APP_BUILD)/$(APP)-unsigned.bin
ifeq ("$(wildcard $(APP_BIN))","")
APP_BIN := $(APP_BUILD)/$(APP).bin
endif
APP_ELF := $(APP_BUILD)/$(APP).elf

.PHONY: clean
clean:
	$(QUIET)rm -rf $(APP_BUILD) $(APP_STAGING) $(APP_STAGING).zip

.PHONY: app
app: $(APP_STAGING_FILES)

#
# Do not make APP_STAGING if NO_REBUILD=y specified
#
ifneq ($(NO_REBUILD),y)
#
# Make the directory before any of the objects in it
#
$(APP_STAGING):
	mkdir -p $@

$(APP_STAGING_FILES): | $(APP_STAGING)
$(APP_STAGING).zip: $(APP_STAGING_FILES)

$(APP_BIN):
	$(QUIET)echo "building $@"; \
	$(IDF_PY) -B $(APP_BUILD) \
		-D BUILD_TYPE=$(BUILD_TYPE) \
		-D BUILD_NAME=$(BUILD_NAME) \
		set-target $(APP_TARGET) \
		app

define KEY_CHECK
	if [ -z "$(SIGNING_KEY)" ]; then \
		echo "SIGNING_KEY is \"$(SIGNING_KEY)\"" >&2; \
		echo "SIGNING_KEY=<key>.pem file parameter required)" >&2; \
		exit 1; \
	fi
endef

$(APP_STAGING)/app.bin: $(APP_BIN)
	$(QUIET)echo signing app; \
	$(call KEY_CHECK); \
	$(ESPSECURE) sign_data --version 2 --keyfile "$(SIGNING_KEY)" \
	    --output $@ $<

$(APP_STAGING)/app.elf: $(APP_ELF)
	$(QUIET)cp $< $@

#
# Boot loader
#

.PHONY: bootloader
bootloader: $(APP_STAGING)/bootloader.bin

ifeq ($(SIGN_BOOT),y)
$(APP_STAGING)/bootloader.bin: $(BOOTLOADER)
	$(QUIET)echo signing bootloader; \
	$(call KEY_CHECK); \
	$(ESPSECURE) sign_data --version 2 --keyfile "$(SIGNING_KEY)" \
	    --output $@ $<; \
	if [ -n "$(SIGNING_KEY2)" ]; then \
		$(ESPSECURE) sign_data --version 2 \
			--keyfile "$(SIGNING_KEY2)" --append_signatures $@; \
	fi; \
	if [ -n "$(SIGNING_KEY3)" ]; then \
		$(ESPSECURE) sign_data --version 2 \
			--keyfile "$(SIGNING_KEY3)" --append_signatures $@; \
	fi

ifneq ($(BOOTLOADER_DEV),)
$(APP_STAGING)/bootloader-dev.bin: $(BOOTLOADER_DEV)
	$(QUIET)echo signing bootloader dev; \
	$(call KEY_CHECK); \
	$(ESPSECURE) sign_data --version 2 --keyfile "$(SIGNING_KEY)" \
	    --output $@ $<; \
	if [ -n "$(SIGNING_KEY2)" ]; then \
		$(ESPSECURE) sign_data --version 2 \
			--keyfile "$(SIGNING_KEY2)" --append_signatures $@; \
	fi; \
	if [ -n "$(SIGNING_KEY3_DEV)" ]; then \
		$(ESPSECURE) sign_data --version 2 \
			--keyfile "$(SIGNING_KEY3_DEV)" \
			--append_signatures $@; \
	fi
endif

else
$(APP_STAGING)/bootloader.bin: $(BOOTLOADER)
	$(QUIET)cp $< $@
endif

#
# Partition table
#
$(APP_STAGING)/partition_table.bin: $(PART_CSV)
	$(GEN_ESPPART) --flash-size $(PART_SIZE)B $< $@
endif # end of ifneq (NO_REBUILD,y)

$(APP_PKG_BUILD)/images/app.bin: $(APP_STAGING)/app.bin
	$(QUIET)cp $< $@

$(APP_PKG_BUILD)/images/bootloader.bin: $(APP_STAGING)/$(BOOTLOADER_SELECT).bin
	$(QUIET)cp $< $@

$(APP_PKG_BUILD)/images/partition_table.bin: $(APP_STAGING)/partition_table.bin
	$(QUIET)cp $< $@

#
# Fix the OEM secret in the config files.  Config files which have the
# string "--your-OEM-secret--" in them where the actual OEM secret will go.
#
# This is to avoid putting OEM secrets in the source repositories.
#
# If PROD_OEM_SECRET is not specified,
# the secret will not be fixed in the config.
#
# If not specified, determine the APP_OEM from the beginning of the
# product name and use the value of OEM_SECRET_xxx where xxx is the OEM.
#
OEM_PLACEHOLDER = "--your-OEM-secret--"
ifeq ($(PROD_OEM_SECRET),)
APP_OEM = $(shell echo $(APP_PROD) | sed -e 's/-.*//')
PROD_OEM_SECRET := $(OEM_SECRET_$(APP_OEM))
endif

.PHONY: FORCE
ifeq ($(PROD_OEM_SECRET),)
$(APP_PKG_BUILD)/prod_config.txt: $(APP_CONF_DIR)/$(APP_CONF) FORCE
	$(QUIET) set -e; \
	grep -q -- $(OEM_PLACEHOLDER) $< && \
		echo "Error: OEM secret not set in config $<" \
		"and PROD_OEM_SECRET not specified" >&2 && exit 1; \
	cp $< $@

$(info PROD_OEM_SECRET not set)
else
$(APP_PKG_BUILD)/prod_config.txt: $(APP_CONF_DIR)/$(APP_CONF) FORCE
	$(QUIET)sed -e "s/$(OEM_PLACEHOLDER)/$(PROD_OEM_SECRET)/"  < $< > $@
endif
