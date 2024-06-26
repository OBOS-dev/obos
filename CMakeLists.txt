# CMakeLists.txt
#
# Copyright (c) 2024 Omar Berrow

cmake_minimum_required(VERSION 3.7)

project(OBOS C ASM_NASM)

# Download dependencies.
# NOTE: As dependencies are added, please add them to the .gitignore file.
# If a dependency cmake file is included, and OBOS_REFRESH_DEPENDENCIES is false, the cmake file shall not download or make any file operations, and only set any cache variables related 
# to the dependency (eg. limine_install, zydis_c, uacpi_cmake_file, etc.)
if (EXISTS ${CMAKE_SOURCE_DIR}/dependencies/needs_download.cmake)
	include (dependencies/needs_download.cmake)
else()
	set (OBOS_REFRESH_DEPENDENCIES true)
endif()
set (OBOSKRNL_EXTERNAL_INCLUDES "${CMAKE_SOURCE_DIR}/dependencies/include")
if (OBOS_REFRESH_DEPENDENCIES)
	file(MAKE_DIRECTORY "dependencies/include")
	include("dependencies/hyper.cmake")
	file (WRITE "${CMAKE_SOURCE_DIR}/dependencies/needs_download.cmake"
		"# This file is auto-generated.\n# Set this variable to true or delete this file to refresh all dependencies.\nset (OBOS_REFRESH_DEPENDENCIES false)"
	)
else()
	if (NOT EXISTS hyper_install)
		include("dependencies/hyper.cmake")
	endif()
endif()

if (NOT DEFINED OUTPUT_DIR)
	set (OUTPUT_DIR ${CMAKE_SOURCE_DIR}/out)
endif()

if (DEFINED OBOS_SLOW_ARTHIMETRIC)
	add_compile_definitions(OBOS_SLOW_ARTHIMETRIC=1)
endif()

set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})

add_compile_definitions(OBOS_ARCHITECTURE_BITS=${OBOS_ARCHITECTURE_BITS})

execute_process(COMMAND
  "${GIT_EXECUTABLE}" describe --match=NeVeRmAtCh --always --abbrev=40 --dirty
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE GIT_SHA1
  ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)

# the date of the commit
execute_process(COMMAND
  "${GIT_EXECUTABLE}" log -1 --format=%ad --date=local
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE GIT_DATE
  ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)

add_compile_definitions (GIT_SHA1="${GIT_SHA1}" GIT_DATE="${GIT_DATE}")

add_subdirectory("src/oboskrnl")
add_subdirectory("src/isogen")

set(C_STANDARD 17)
set(C_STANDARD_REQUIRED true)