#
# Copyright 2022 Ayla Networks, Inc.  All rights reserved.
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

export APP ?= air_purifier
export APP_PKG_VER ?= 1.0.3

QUIET := @
NULL :=

APP_CONFS ?= \
	hm-defender \
	$(NULL)

#
# Part numbers for selecting signing keys.
#
APP_PART_esp32c3 := AY028MCA

#
# Use target-specific variable values to override defaults for targets
# These targets are in the included makefile.
#
hm-defender: export BOOTLOADER = ../../../esp32c3/prebuilt/bootloader-sec-4M.bin
hm-defender: export PART_SIZE = 4M

include ../../make/pha_release_v2.1.mk
