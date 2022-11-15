#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#

PROD_PKG_VER = 1.3.1
PROD_PKG_NAME = Ayla_esp32_prod_pkg
PROD_PKG_FULL_NAME = Ayla-esp32-prod_pkg-$(PROD_PKG_VER)

define HELP_TEXT
Build archive for PHA production package verison $(PROD_PKG_VER)

Targets:
	prod_pkg:	production package only (default)
	app_pkg:	image files for app: use app.mk

options, with defaults shown for prod_pkg:
	PROD_PKG_VER=$(PROD_PKG_VER)

For app_pkg help use: 'make -f app.pkg help'
endef

#
# relative directory names
# TOP = top of the workspace tree
#
TOP ?= ../../..
DIR ?= arch/esp32/prod
NULL :=
QUIET ?= @
LIB = pyayla
CONF_DIR = ../conf

GEN_ESPPART = $(IDF_PATH)/components/partition_table/gen_esp32part.py
NVS_PARTGEN_DIR = components/nvs_flash/nvs_partition_generator
ESPSECURE = $(IDF_PATH)/components/esptool_py/esptool/espsecure.py

#
# default target (because it's first)
#
.PHONY: default
default: prod_pkg
	$(info For help, use 'make help')

.PHONY: help
help:
	@:
	$(info $(HELP_TEXT))

BUILD = $(TOP)/build
PROD_PKG = $(TOP)/build/$(PROD_PKG_FULL_NAME)

.PHONY: prod_pkg
prod_pkg: $(PROD_PKG).zip

%.zip: %
	$(QUIET)echo Archiving $(notdir $@); \
	rm -f $@ && \
	cd $< && \
	zip -q -r ../$(notdir $@) . && \
	echo successful build result: $@

PROD_LIB_CONTENTS = \
	$(LIB)/__init__.py	\
	$(LIB)/ayla_conf_tokens.py	\
	$(LIB)/prod_batch.py	\
	$(LIB)/prod_csv.py 	\
	$(LIB)/prod_expect.py 	\
	$(LIB)/prod_install.py	\
	$(TOP)/oem/oem_config.py \
	$(NULL)

PROD_PKG_CONTENTS = \
	README.txt	\
	prod_run.py 	\
	prod_tool.py 	\
	$(NULL)

.PHONY: FORCE
$(PROD_PKG): FORCE
	$(QUIET)echo making $(notdir $@); \
	rm -rf $@; \
	set -e; \
	mkdir -p $@; \
	mkdir -p $@/ext; \
	mkdir -p $@/esp-idf/components/esptool_py; \
	cp -R $(IDF_PATH)/components/esptool_py/esptool $@/esp-idf/components/esptool_py; \
	find $@/esp-idf/components -type d \
		\( -name __pycache__ -o -name test \) -print | xargs rm -r ; \
	cp $(IDF_PATH)/tools/mass_mfg/mfg_gen.py $@/ext; \
	mkdir -p $@/esp-idf/${NVS_PARTGEN_DIR}; \
	cp $(IDF_PATH)/${NVS_PARTGEN_DIR}/nvs_partition_gen.py \
		$@/esp-idf/${NVS_PARTGEN_DIR}; \
	mkdir -p $@/$(LIB); \
	cp $(PROD_LIB_CONTENTS) $@/$(LIB); \
	cp $(PROD_PKG_CONTENTS) $@

#
# For compatibility
#
.PHONY: app_pkg
app_pkg:
	$(MAKE) -f app_pkg.mk
