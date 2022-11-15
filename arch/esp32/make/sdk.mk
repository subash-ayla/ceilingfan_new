#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#
QUIET ?= @

#
# User should set ESP_IDF to the absolute path for ESP_IDF including ADA source.
# This should be done in the environment or on the make command line.
#
ifeq ($(IDF_PATH),)
$(error set IDF_PATH to the full pathname of the ESP-IDF with ADA)
endif

#
# Build type may be "DEV" or "REL" (short for development or release).
# Release builds will cause boot security to be enabled.
# This goes into app_build.h.
#
BUILD_TYPE ?= DEV
ifneq ($(BUILD_TYPE),REL)
ifneq ($(BUILD_TYPE),DEV)
$(error BUILD_TYPE should be REL or DEV)
endif
endif

#
# configuration to use
#
SDK_MK_SDKCONFIG := $(abspath $(TOP)/arch/esp32/conf/sdkconfig-4M-dev)
SDKCONFIG ?= $(SDK_MK_SDKCONFIG)

#
# Generated header file with versioning information
#
BUILD_H = build/include/app_build.h

#
# Set only the common required symbols here.
# Anything optional should be set in the including Makefile.
# Keep these sorted alphabetically.
#
CFLAGS += \
	-DAYLA_WIFI_SUPPORT \
	-DCONF_NO_ID_FILE \
	-DESP32 \
	-DLOG_LOCK \
	-DLOG_SEV_SHORT \
	-DLWIP_1_5_0_SUPPORT \
	$(NULL)

#
# Skip cstyle and hooks in released source packages
#
ifeq ($(wildcard $(TOP)/util/hooks),)
CSTYLE_HOOKS =
else
CSTYLE_HOOKS = hooks cstyle
endif

.PHONY: default
default: $(BUILD_H) $(CSTYLE_HOOKS) all

ifneq ($(wildcard $(TOP)/util/hooks),)
.PHONY: hooks FORCE
hooks: FORCE
	make -C $(TOP) hooks-install
endif

ifneq ($(wildcard $(TOP)/util/cstyle_ayla),)
CSTYLE_DIRS += main ../../components/libapp
CSTYLE_BUILD = $(TOP)/build/cstyle/$(DIR)
include $(TOP)/arch/esp32/make/cstyle.mk
endif

include $(TOP)/arch/esp32/make/version.mk
include $(TOP)/arch/esp32/make/ada_check.mk

#
# If selected, apply patches to LwIP in IDF_PATH for Firedome
#
ifeq ($(MAKE_SDK_INCLUDE_FIREDOME),y)
include $(TOP)/ext/firedome/make/project_firedome.mk
endif

include $(IDF_PATH)/make/project_ayla.mk
