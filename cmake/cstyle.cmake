#
# Copyright 2021 Ayla Networks, Inc.  All rights reserved.
#
file(TO_NATIVE_PATH "${REPO_ROOT}/util/cstyle.py" CSTYLE)

function(add_cstyle_checks LIB_TARGET SOURCES)
	list(SORT SOURCES)
	list(REMOVE_DUPLICATES SOURCES)
	add_custom_target(${LIB_TARGET}_cstyle
		COMMAND "${CMAKE_COMMAND}" -E chdir
			"${CMAKE_CURRENT_SOURCE_DIR}"
			env REPO_ROOT=${REPO_ROOT}
			"${CSTYLE}" --cache ${CMAKE_CURRENT_BINARY_DIR}/cstyle
				--cmd ${REPO_ROOT}/util/cstyle_ayla
				${SOURCES}
		DEPENDS ${SOURCES}
		VERBATIM
	)
	add_dependencies(${LIB_TARGET} ${LIB_TARGET}_cstyle)
endfunction()
