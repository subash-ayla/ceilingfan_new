#
# Copyright 2022 Ayla Networks, Inc.  All rights reserved.
#

if(DEFINED ADA_REQUIRED_VERSION OR DEFINED ADA_REQUIRED_REV
		OR DEFINED ADA_MIN_VERSION)
	set(ADA_PATH ${IDF_PATH}/components/ayla)
	set(ADA_BUILD_FILE ${ADA_PATH}/ada/build.h)

	set(ADA_VERSION unknown)
	set(ADA_REV unknown)

	#
	# Read header file into list.  Parse desired items in list
	#
	file(STRINGS ${ADA_BUILD_FILE} BUILD_STRINGS REGEX "^#define ")
	foreach(line ${BUILD_STRINGS})
		string(REPLACE "#define " "" line "${line}")
		string(REGEX REPLACE " .*" "" name "${line}")
		string(REGEX REPLACE "[^ ]* " "" val "${line}")
		string(REGEX REPLACE "\"" "" val "${val}")
		if("${name}" STREQUAL "ADA_VERSION")
			set(ADA_VERSION ${val})
			string(REGEX MATCH
				"([^.-]*)\.([^.-]*)[.]*([^.-]*)"
				valm ${val})
			set(ADA_VER_MAJOR ${CMAKE_MATCH_1})
			set(ADA_VER_MINOR ${CMAKE_MATCH_2})
			if(DEFINED CMAKE_MATCH_3)
				set(ADA_VER_MICRO ${CMAKE_MATCH_3})
			else()
				set(ADA_VER_MICRO 0)
			endif()
		elseif("${name}" STREQUAL "BUILD_VERSION")
			set(ADA_REV ${val})
		endif()
	endforeach()
	if(DEFINED ADA_REQUIRED_REV)
		if(NOT "${ADA_REQUIRED_REV}" STREQUAL "${ADA_REV}")
			message(SEND_ERROR
			 "ADA revision ${ADA_REQUIRED_REV} required. Have ${ADA_REV}")
		endif()
	endif()
	if(DEFINED ADA_REQUIRED_VERSION)
		if(NOT "${ADA_REQUIRED_VERSION}" STREQUAL "${ADA_VERSION}")
			message(SEND_ERROR
		    "ADA GIT revision ${ADA_REQUIRED_VERSION} required. Have ${ADA_VERSION}")
		endif()
	endif()
	if(DEFINED ADA_MIN_VERSION)
		string(REGEX MATCH
			"([^.-]*)\.([^.-]*)[.]*([^.-]*)"
			valm ${ADA_MIN_VERSION})
		set(MIN_MAJOR ${CMAKE_MATCH_1})
		set(MIN_MINOR ${CMAKE_MATCH_2})
		if(DEFINED CMAKE_MATCH_3)
			set(MIN_MICRO ${CMAKE_MATCH_3})
		else()
			set(MIN_MICRO 0)
		endif()
		set(fail 0)
		if(${MIN_MAJOR} GREATER ${ADA_VER_MAJOR})
			set(fail 1)
		elseif((${MIN_MAJOR} EQUAL ${ADA_VER_MAJOR}) AND
				(${MIN_MINOR} GREATER ${ADA_VER_MINOR}))
			set(fail 1)
		elseif((${MIN_MAJOR} EQUAL ${ADA_VER_MAJOR}) AND
				(${MIN_MINOR} EQUAL ${ADA_VER_MINOR}) AND
				("${MIN_MICRO}" GREATER "${ADA_VER_MICRO}"))
			set(fail 1)
		endif()
		if(${fail} EQUAL 1)
			message(SEND_ERROR "ADA minimum version ${ADA_MIN_VERSION} required.  Have ${ADA_VERSION}")
		endif()
	endif()
endif()
