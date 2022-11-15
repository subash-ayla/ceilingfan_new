#
# Copyright 2022 Ayla Networks, Inc.  All rights reserved.
#

#
# Add project definitions needed for Firedome and include ayla_project.cmake.
#
# This should be included from the top level CMakeLists.txt in the host app.
#
# It applies Firedome patches and then includes ayla_project.cmake which
# then includes the ESP_IDF project.cmake.
#
# Apps not requiring Firedome should include ayla_project.cmake directly.
#

include($ENV{IDF_PATH}/tools/cmake/ayla_project.cmake)

get_filename_component(FIREDOME_PATH
	${REPO_ROOT}/ext/firedome
	ABSOLUTE)

list(APPEND EXTRA_COMPONENT_DIRS ${FIREDOME_PATH}/component)

#
# Include Firedome patches to LwIP
#
include(${FIREDOME_PATH}/patches.cmake)

#
# Set component value for LwIP to require.
#
idf_build_set_property(FIREDOME_COMPONENT firedome)

idf_build_set_property(COMPILE_OPTIONS "-DAYLA_FIREDOME_SUPPORT" APPEND)
