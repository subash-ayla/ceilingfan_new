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

export SIGNING_KEY_esp32 = \
	$(SIGNING_KEY_DIR)/$(APP_PART_esp32)/$(SIGNING_KEY).pem
export SIGNING_KEY2_esp32 =
export SIGNING_KEY3_esp32 =

export SIGNING_KEY_esp32c3 = \
	$(SIGNING_KEY_DIR)/$(APP_PART_esp32c3)/$(SIGNING_KEY).pem
export SIGNING_KEY2_esp32c3 = \
	$(SIGNING_KEY_DIR)/$(APP_PART_esp32c3)/rel2.pem
export SIGNING_KEY3_esp32c3 =

#
# Relative path to root of repository.
# This is the case for most apps that include this.
#
TOP ?= ../../../..
APP_PKG_MK ?= $(TOP)/arch/esp32/make/app_pkg.mk

#
# Rules for building the app and app packages
# $1 is the target, esp32 or esp32c3.
# $2 is the build type, REL or DEV
# $3 is the build name, "" or -eng,
# $4 is the partition table version
#
define APP_RULES
	echo; \
	echo target $1 confs: $(APP_CONF_$1); \
	set -e; \
	for conf in $(APP_CONF_$1); \
	do \
		echo making $(APP) $$conf; \
		make -f $(APP_PKG_MK) app_pkg \
			APP_CONF=$$conf.txt \
			APP=$(APP) \
			APP_PROD=$$conf \
			SIGN_BOOT=y \
			SIGNING_KEY=$(SIGNING_KEY_$1) \
			SIGNING_KEY2=$(SIGNING_KEY2_$1) \
			SIGNING_KEY3=$(SIGNING_KEY3_$1) \
			APP_TARGET=$1 \
			BUILD_TYPE=$2 \
			BUILD_NAME=$3 \
			PARTITION_VER=$4; \
	done
endef

.PHONY: default
default: dev

.PHONY: clean
clean:
	rm -rf build

.PHONY: release
release:
	$(QUIET)echo "making release app packages for $(APP) $(APP_PKG_VER)"; \
	set -e; \
	$(call APP_RULES,esp32,REL,"",v1); \
	$(call APP_RULES,esp32c3,REL,"",v2); \
	echo release build complete

.PHONY: dev
dev:
	$(QUIET)echo "making dev app packages for $(APP) $(APP_PKG_VER)"; \
	set -e; \
	$(call APP_RULES,esp32,DEV,eng,v1); \
	$(call APP_RULES,esp32c3,DEV,eng,v2); \
	echo dev build complete
