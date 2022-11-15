#
# Copyright 2018 Ayla Networks, Inc.  All rights reserved.
#

#
# Rules for C style checking.
#
# Checks the style of each target file in $(CSTYLE_SOURCES) and touches a
# tracking file named %.cs.  As long as that file is newer, the style check
# is not repeated.
#
# Set CSTYLE_SOURCES to the list of sources, relative to the including Makefile.
# You may set CSTYLE to an alternative program which takes the pathname
# as its argument.
# Optionally set CSTYLE_BUILD to the place to put reference info
#
CSTYLE ?= $(TOP)/util/checkpatch_ayla \
		--strict --terse --summary-file --no-tree -f

CSTYLE_BUILD ?= $(TOP)/build/cstyle
CSTYLE_BUILD := $(strip $(CSTYLE_BUILD))

#
# Files in directories
# These are relative to the $(APP) directory
#
CSTYLE_SOURCES += $(foreach dir, $(CSTYLE_DIRS), \
	$(shell find $(dir) -path '*/build' -prune -o -name '*.[ch]' -print))

#
# Filter cstyle source
#
CSTYLE_SOURCES := $(filter %.c %.h, $(CSTYLE_SOURCES))

#
# List of targets in build directory, relative to $(TOP), remove duplicates
#
CSTYLE_ABS := $(abspath $(CSTYLE_SOURCES))
TOP_ABS := $(abspath $(TOP))
CSTYLE_SOURCES = $(CSTYLE_ABS:$(TOP_ABS)/%=%)
CSTYLE_TARGETS := $(CSTYLE_SOURCES:%=$(CSTYLE_BUILD)/%.cs)
CSTYLE_TARGETS := $(sort $(CSTYLE_TARGETS))

#
# Rule for each file to be checked.
#
$(CSTYLE_BUILD)/%.cs: $(TOP_ABS)/%
	$(QUIET)echo CSTYLE $(<:$(PWD)/%=%); \
	$(CSTYLE) $(<:$(PWD)/%=%) && (mkdir -p $(dir $@); touch $@)

#
# Check all files
#
.PHONY: cstyle
cstyle: $(CSTYLE_TARGETS)

#
# Remove all tracking files.
#
.PHONY: clean_cstyle
clean_cstyle:
	$(QUIET)echo "Clean cstyle staging folder."; rm -rf $(CSTYLE_BUILD)
