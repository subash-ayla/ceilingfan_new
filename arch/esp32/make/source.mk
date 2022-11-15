#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#

TOP = ../../..
DIR = arch/esp32/make

# Select app for source package.
# Default is se_app

APP ?= se_app

ifeq ($(VER),)
# $(info Optionally specify another application as APP=<app-name>)
$(error Specify $(APP) package version as VER=<version> on the command line)
endif

ARCH ?= esp32
ARCH_DIR = arch/$(ARCH)

APP_PATH ?= $(ARCH_DIR)/apps/$(APP)

BUILD = $(TOP)/build

SRC_NAME = Ayla-$(ARCH)-$(APP)-$(VER)-src
SRC_PKG = $(BUILD)/$(SRC_NAME)
SRC_PKG_TAR = $(SRC_PKG).tgz

QUIET ?= @
NULL :=

.PHONY: source src_pkg
source src_pkg: $(SRC_PKG_TAR)

$(SRC_PKG_TAR): $(SRC_PKG)
	$(QUIET)echo Creating archive $@; \
	echo Archive $(abspath $@); \
	cd $(BUILD) && tar cfz $(SRC_NAME).tgz $(SRC_NAME)

APP_SOURCES = \
	.gitignore \
	CMakeLists.txt \
	main \
	www \
	sdkconfig.defaults \
	sdkconfig.defaults.esp32 \
	sdkconfig.defaults.esp32c3 \
	$(NULL)

SRC_PKG_PATHS = \
	$(ARCH_DIR)/conf/flash_partitions_4M.csv \
	$(ARCH_DIR)/conf/flash_partitions_8M.csv \
	$(ARCH_DIR)/conf/flash_partitions_v2_4M.csv \
	util/checkpatch_ayla \
	util/cstyle.py \
	util/cstyle_ayla \
	$(NULL)

ifeq ($(APP),se_app)
APP_SOURCES += sdkconfig
endif
ifeq ($(APP),starter_app)
SRC_PKG_PATHS += $(ARCH_DIR)/conf/sdkconfig-4M-dev
endif
ifeq ($(FIREDOME),y)
SRC_PKG_PATHS += ext/firedome
endif

.PHONY: FORCE
$(SRC_PKG): FORCE
	$(QUIET)echo Copying sources; \
	set -e; \
	rm -rf $@; \
	mkdir -p $@/$(APP_PATH); \
	cp $(TOP)/.gitignore $@; \
	cp -r $(addprefix $(TOP)/$(APP_PATH)/,$(APP_SOURCES)) $@/$(APP_PATH); \
	mkdir -p $@/$(ARCH_DIR)/components; \
	cp -r $(TOP)/$(ARCH_DIR)/components/libapp $@/$(ARCH_DIR)/components; \
	cp -r $(TOP)/cmake $@/cmake; \
	$(foreach pkg, $(SRC_PKG_PATHS), \
	    mkdir -p $@/$(dir $(pkg)) && \
	    cp -R $(TOP)/$(pkg) $@/$(dir $(pkg));) \
	mkdir -p $@/$(ARCH_DIR)/conf; \
	cp $(TOP)/$(ARCH_DIR)/conf/flash_partitions_8M.csv $@/$(ARCH_DIR)/conf
