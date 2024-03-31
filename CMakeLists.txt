# CMakeLists.txt
#
# Copyright (c) 2024 Omar Berrow

cmake_minimum_required(VERSION 3.7)

project(OBOS C CXX ASM_NASM)

# Download dependencies.
# NOTE: As dependencies are added, please add them to the .gitignore file.
if (EXISTS ${CMAKE_SOURCE_DIR}/dependencies/needs_download.cmake)
	include (dependencies/needs_download.cmake)
else()
	set (OBOS_REFRESH_DEPENDENCIES true)
endif()
set (OBOSKRNL_EXTERNAL_INCLUDES "${CMAKE_SOURCE_DIR}/dependencies/include")
if (OBOS_REFRESH_DEPENDENCIES)
	file(MAKE_DIRECTORY "dependencies/include")
	if (OBOS_ARCHITECTURE STREQUAL "x86_64")
		include("dependencies/limine.cmake")
	endif()
	include ("dependencies/uacpi.cmake")
	file (WRITE "${CMAKE_SOURCE_DIR}/dependencies/needs_download.cmake" 
		"# This file is auto-generated.\n# Set this variable to true or delete this file to refresh all dependencies.\nset (OBOS_REFRESH_DEPENDENCIES false)"
	)
endif()

if (NOT DEFINED OUTPUT_DIR)
	set (OUTPUT_DIR ${CMAKE_SOURCE_DIR}/out)
endif()

set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})

add_subdirectory("src/libs/uacpi")
add_subdirectory("src/oboskrnl")
add_subdirectory("src/isogen")