#
# Copyright 2021 Ayla Networks, Inc.  All rights reserved.
#

# Check the ADA version in IDF_PATH against the required version
# or tag mentioned in ada_required.mk.

ADA_REQ_FILE = $(TOP)/arch/esp32/make/ada_required.mk

ADA_BUILD_FILE = $(IDF_PATH)/components/ayla/ada/build.h

include $(ADA_REQ_FILE)
include $(IDF_PATH)/components/ayla/ada/ada_version.mk

VERSION_ERROR =

ifneq ($(ADA_REQUIRED_VERSION),)
ifneq ($(ADA_VERSION),$(ADA_REQUIRED_VERSION))
VERSION_ERROR += Requires ADA version $(ADA_REQUIRED_VERSION) has $(ADA_VERSION).
endif
endif

ifneq ($(ADA_REQUIRED_REV),)
ADA_BUILD_VERSION = $(lastword $(shell grep BUILD_VERSION $(ADA_BUILD_FILE)))
ifneq ("$(ADA_REQUIRED_REV)",$(ADA_BUILD_VERSION))
VERSION_ERROR += Requires ADA git ID "$(ADA_REQUIRED_REV)" has $(ADA_BUILD_VERSION).
endif
endif

ifneq ($(VERSION_ERROR),)
$(error ADA requirement error: $(VERSION_ERROR) \
	To set required version, edit $(ADA_REQ_FILE))
endif
