#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#
# make version header file
#
BUILD_H ?= build.h
BUILD_PREFIX ?= BUILD
BUILD_NAME ?= eng
ifneq ($(BUILD_NAME),)
BUILD_SUFFIX ?= "-$(BUILD_NAME)"
else
BUILD_SUFFIX ?= ""
endif

APP_NAME ?= $(APP)
APP_VER ?= undefined

QUIET ?= @

#
# Source code management system revision info
#
SCM_REV := $(shell git rev-parse --verify --short HEAD)
SCM_PLUS := $(shell git diff-index --quiet HEAD || echo +)
SCM_REV := $(SCM_REV)$(SCM_PLUS)
ifneq ($(BUILD_NAME)$(SCM_PLUS), $(filter $(BUILD_NAME)$(SCM_PLUS), -beta  ))

#
# Non-release builds get workspace label (if present) and user-ID added to rev.
#
ifneq ($(BUILD_TYPE),REL)
ifneq ($(WORKSPACE_LABEL),)
SCM_REV := $(WORKSPACE_LABEL)/$(SCM_REV)
endif
SCM_REV := $(USER)/$(SCM_REV)
endif
endif

#
# Make a header file indicating the build information.
#
.PHONY: FORCE
$(BUILD_H): FORCE
	$(QUIET)( \
		echo "#ifndef $(BUILD_PREFIX)_DATE"; \
		date "+#define $(BUILD_PREFIX)_DATE \"%F\""; \
		date "+#define $(BUILD_PREFIX)_TIME \"%T\""; \
		echo "#define $(BUILD_PREFIX)_TYPE \""$(BUILD_TYPE)"\""; \
		echo "#define $(BUILD_PREFIX)_TYPE_$(BUILD_TYPE)"; \
		echo "#define $(BUILD_PREFIX)_VERSION \""$(SCM_REV)"\""; \
		echo "#define $(BUILD_PREFIX)_SUFFIX \"$(BUILD_SUFFIX)\""; \
		echo "#endif"; \
		echo "#ifndef APP_NAME"; \
		echo "#define APP_NAME \""$(APP_NAME)"\""; \
		echo "#endif"; \
		echo "#ifndef APP_VER"; \
		echo "#define APP_VER \""$(APP_VER)"\""; \
		echo "#endif" \
	) > $@
