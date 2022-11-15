#
# Copyright 2022 Ayla Networks, Inc.  All rights reserved.
#

#
# Makefile to be included from apps/*/pkg.mk to build app packages
# This uses app_pkg.mk
#

#
# Make release app package
#
# Before using this for a release build, include the following settings on
# the command line or in the environment:
#
# SIGNING_KEY_DIR=<path containing AY028MLB/rel1.pem and AY028MCB/rel1.pem>
# OEM_SECRET_ayla=<OEM secret key for Ayla OEM>
# OEM_SECRET_ctc=<OEM secret key for Canadian Tire OEM>
#
# The name of the config file up to the first hyphen selects OEM_SECRET
# variable, e.g. ayla-bulb-* will use OEM_SECRET_ayla.
#

#
# Signing keys depend on personal setup and may come from the environment.
#
export SIGNING_KEY ?= rel1

export SIGNING_KEY_esp32 ?= \
	$(SIGNING_KEY_DIR)/$(APP_PART_esp32)/$(SIGNING_KEY).pem
export SIGNING_KEY2_esp32 ?=
export SIGNING_KEY3_esp32 ?=

export SIGNING_KEY_esp32c3 ?= \
	$(SIGNING_KEY_DIR)/$(APP_PART_esp32c3)/$(SIGNING_KEY).pem
export SIGNING_KEY2_esp32c3 ?= \
	$(SIGNING_KEY_DIR)/$(APP_PART_esp32c3)/rel2.pem
export SIGNING_KEY3_esp32c3 ?=

#
# Relative path to root of repository.
# This is the case for most apps that include this.
#
TOP ?= ../../../..
APP_PKG_MK ?= $(TOP)/arch/esp32/make/app_pkg.mk

.PHONY: default
default: dev

.PHONY: clean
clean:
	$(QUIET)rm -rf build

.PHONY: dev release
dev release: $(APP_CONFS)

export APP_TARGET ?= esp32
export PARTITION_VER ?= v1
export SIGN_BOOT ?= y

release : export BUILD_TYPE = REL
release : export BUILD_NAME ?=

export BUILD_TYPE ?= DEV
export BUILD_NAME ?= eng

APP_PKG_MK = ../../make/app_pkg.mk

#
# Build for each configuration.  Note exported variables are passed into submake.
# Note that some variables may be overridden per-target by the including Makefile.
#
.PHONY: $(APP_CONFS)
$(APP_CONFS):
	$(QUIET)echo "Making dev app packages for $(APP) $(APP_PKG_VER)"; \
	$(MAKE) -f $(APP_PKG_MK) \
		APP_CONF=$@.txt \
		APP_PROD=$@ \
		SIGNING_KEY=$(SIGNING_KEY_$(APP_TARGET)) \
		SIGNING_KEY2=$(SIGNING_KEY2_$(APP_TARGET)) \
		SIGNING_KEY3=$(SIGNING_KEY3_$($APP_TARGET)) \
		$(NULL)
