#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#

#
# Makefile for Productized Host Applications (PHA) bootloader on esp32 rev 3.
#
# This bootloader enables secure_boot_v2 only after flash encryption is
# in release mode as set by the app.
#
APP = pha_boot
PROJECT_NAME := $(APP)

SIGNING_KEY ?= $(abspath ../../conf/eng_key.pem)

.PHONY: default
default: ayla_bootloader

NULL :=
QUIET := @

# specify module type and size
MODULE ?= esp_wroom_32e
ifeq (,$(wildcard conf/$(MODULE).mk))
$(error Unsupported module type: $(MODULE))
endif

include conf/$(MODULE).mk
FLASH_SIZE ?= 4M

SDKCONFIG ?= $(abspath conf/sdkconfig-$(MODULE)-$(FLASH_SIZE))
ifeq (,$(wildcard $(SDKCONFIG)))
$(error Missing sdkconfig file $(SDKCONFIG))
endif

BUILD_DIR_BASE = $(abspath build/$(MODULE)-$(FLASH_SIZE))
ESPTOOL = python $(IDF_PATH)/components/esptool_py/esptool/esptool.py
ESPSECURE = python $(IDF_PATH)/components/esptool_py/esptool/espsecure.py
IDF_PY = python $(IDF_PATH)/tools/idf.py

#
# Add patch before building ESP-IDF bootloader target.
#
BOOT_PATCH_DONE = $(IDF_PATH)/components/bootloader_support/Ayla_pha_patch_done

$(BOOT_PATCH_DONE): boot_sec_patch.txt
	$(QUIET)echo patching bootloader; \
	(cd $(IDF_PATH) && patch -p1 && touch $@) < $<

.PHONY: ayla_bootloader
ayla_bootloader: $(BOOT_PATCH_DONE) $(BUILD_DIR_BASE)/ayla_bootloader-signed.bin

IDF_PY_ARGS = \
	-D "SDKCONFIG=$(SDKCONFIG)" \
	-B $(BUILD_DIR_BASE) \
	$(NULL)

.PHONY: FORCE
$(BUILD_DIR_BASE)/bootloader/bootloader.elf: FORCE
	$(QUIET)echo "Building bootloader for $(CHIP)"; \
	set -e; \
	test -d $(BUILD_DIR_BASE) || \
		$(IDF_PY) $(IDF_PY_ARGS) set-target $(CHIP)
	$(IDF_PY) $(IDF_PY_ARGS) bootloader

$(BUILD_DIR_BASE)/ayla_bootloader.bin: $(BUILD_DIR_BASE)/bootloader/bootloader.elf
	$(QUIET)echo "Building for $(CHIP) with flash: size:$(FLASH_SIZE)B" \
		"freq:$(FLASH_FREQ)Hz mode:$(FLASH_MODE)"
	$(ESPTOOL) --chip $(CHIP) elf2image --output $@ \
		--flash_freq $(FLASH_FREQ) --flash_mode $(FLASH_MODE) \
		--flash_size $(FLASH_SIZE)B $<

$(BUILD_DIR_BASE)/ayla_bootloader-signed.bin: $(BUILD_DIR_BASE)/ayla_bootloader.bin
	$(QUIET)echo signing with $(notdir $(SIGNING_KEY)); \
	$(ESPSECURE) sign_data --keyfile $(SIGNING_KEY) --version 2 \
		--output $@ $<

clean:
	rm -rf build
