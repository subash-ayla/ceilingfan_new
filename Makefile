#
# Copyright 2020 Ayla Networks, Inc.  All rights reserved.
#

#
# Relative directory locations
# TOP is required by included makefiles to be the top of the workspace.
# DIR is this directory's path from $(TOP)
#
TOP := .
DIR := .

NULL :=
QUIET ?= @

#
# Default architecture (under arch directory).
# Only esp32 supported for now.
#
ARCH ?= esp32

#
# List of apps to make by default
#
APPS ?= \
	starter_app \
	$(NULL)

#
# Default rule: make all apps
#
.PHONY: default
default: hooks-install $(APPS)

#
# Style checking rules
#
CSTYLE_DIRS = \
	arch/esp32/components \
	arch/esp32/apps \
	$(NULL)

include arch/$(ARCH)/make/cstyle.mk

#
# Rule to make any app
#
.PHONY: FORCE
.PHONY: $(APPS)
$(APPS): cstyle FORCE
	$(MAKE) -C arch/$(ARCH)/apps/$@

#
# Clean the build trees
#
.PHONY: clean
clean: FORCE
	$(QUIET)rm -rf \
		build \
		arch/$(ARCH)/apps/*/build \
		$(NULL)

#
# Install .git/hooks from util/hooks
#
HOOKS_DIR = util/hooks
HOOKS = $(shell echo $(HOOKS_DIR)/*)
GIT_HOOKS = $(HOOKS:$(HOOKS_DIR)/%=.git/hooks/%)

.git/hooks/%: $(HOOKS_DIR)/%
	$(QUIET)echo INSTALL $@; \
	cp $< $@ && \
	chmod +x $@

.PHONY: hooks-install
hooks-install: $(GIT_HOOKS)

#
# Source package
#
.PHONY: source src_pkg
source src_pkg: FORCE
	$(MAKE) -C arch/esp32/make -f source.mk

.PHONY: firedome_pkg
firedome_pkg: FORCE
	$(MAKE) -C arch/esp32/make -f source.mk APP=starter_app FIREDOME=y
