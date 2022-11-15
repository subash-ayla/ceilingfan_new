#
# Copyright 2021 Ayla Networks, Inc.  All rights reserved.
#

#
# Generate build.h file in the current binary directory.
#
STRING(TIMESTAMP TIME "%Y-%m-%d;%H:%M:%S")
list(GET TIME 0 BUILD_DATE)
list(GET TIME 1 BUILD_TIME)
set(BUILD_PREFIX BUILD)

if(NOT DEFINED BUILD_TYPE)
	set(BUILD_TYPE "DEV")
elseif(     (NOT "${BUILD_TYPE}" STREQUAL "REL") AND
	    (NOT "${BUILD_TYPE}" STREQUAL "DEV")
	)
	message(FATAL_ERROR "BUILD_TYPE must be REL or DEV")
endif()

if(NOT DEFINED BUILD_NAME)
	set(BUILD_NAME "eng")
endif()
if(NOT "${BUILD_NAME}" STREQUAL "")
	set(BUILD_SUFFIX "-${BUILD_NAME}")
else()
	set(BUILD_SUFFIX "")
endif()

#
# Find the GIT ID (SCM_REV).
#
execute_process(COMMAND
		git rev-parse --verify --short HEAD
		OUTPUT_VARIABLE SCM_REV
	)
string(STRIP "${SCM_REV}" SCM_REV)

#
# Append a + to SCM_REV if there are any differences in the tree
#
execute_process(COMMAND
		git diff-index HEAD
		OUTPUT_VARIABLE SCM_PLUS
	)
string(LENGTH "${SCM_PLUS}" SCM_PLUS)
if(${SCM_PLUS})
	set(SCM_REV "${SCM_REV}+")
endif()

#
# generate build.h from template in cmake/build.h.in
#
file(TO_CMAKE_PATH ${REPO_ROOT}/cmake/build.h.in BUILD_H_IN)
set(BUILD_H app_build.h)
configure_file(${BUILD_H_IN} ${BUILD_H})

#
# Target to depend on the file
#
add_custom_target(${COMPONENT_LIB}_build_info
		DEPENDS ${BUILD_H_IN}
	)
add_dependencies(${COMPONENT_LIB} ${COMPONENT_LIB}_build_info)

#
# Add the build directory to the include path for this component
#
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
