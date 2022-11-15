#
# Copyright 2022 Ayla Networks, Inc.  All rights reserved.
#

#
# Common actions for makefiles in fast-track apps using libpha
#
target_compile_options(${COMPONENT_LIB} PRIVATE
		-D_HAS_ASSERT_F_
		-DAYLA_ESP32_SUPPORT
		-DLWIP_1_5_0_SUPPORT
		-Wno-missing-field-initializers
	)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${REPO_ROOT}/cmake/version.cmake)
include(${REPO_ROOT}/cmake/cstyle.cmake)
add_cstyle_checks(${COMPONENT_LIB} "${CSTYLE_SOURCES}")
