#
# Copyright 2022 Ayla Networks, Inc.  All rights reserved.
#

#
# This should be included from the top level CMakeLists.txt in the host app.
# It applies patches and then includes ESP-IDF project.cmake.
#

#
# Get list of patches lists.
#
set(patch_cmake $ENV{IDF_PATH}/patches/patches.cmake)

#
# Add firedome patches if selected.
#
if(${AYLA_FIREDOME_SUPPORT}) 
	list(APPEND patch_cmake ${REPO_ROOT}/ext/firedome/patches.cmake)
endif()

#
# Apply patches to ESP-IDF. 
#
set(patch_cmd patch)

#
# Apply patches in directory from list, if not already applied
# The first argument is the base directory where the patches should be applied
# The second argument is the directory containing the patch files
# The following arguments are the patch file names.
#
function(apply_patches)
	if(${ARGC} LESS 3)
		message(FATAL_ERROR "apply_patches: insufficient args")
	endif()
	list(POP_FRONT ARGV patch_root patch_dir)
	foreach(patch ${ARGV})
		set(done_name ${patch})
		list(TRANSFORM done_name PREPEND "${patch_root}/.patch-")
		list(TRANSFORM done_name APPEND "-done")

		if (NOT EXISTS ${done_name})
			message("patch: applying patch ${patch}")
			execute_process(
				COMMAND ${CMAKE_COMMAND}
					-E chdir "${patch_root}"
					"${patch_cmd}" --forward -p1
					-i "${patch_dir}/${patch}"
				RESULT_VARIABLE status
			)
			message("patch: status ${status}")
			if(${status} EQUAL 0)
				execute_process(
					COMMAND ${CMAKE_COMMAND}
						-E touch "${done_name}"
				)
			endif()
		endif()
	endforeach()
endfunction()

