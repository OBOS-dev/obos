# CMakeLists.txt
#
# Copyright (c) 2024 Omar Berrow

cmake_minimum_required(VERSION 3.27)

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
		include("dependencies/hyper.cmake")
	endif()
	file (WRITE "${CMAKE_SOURCE_DIR}/dependencies/needs_download.cmake" 
		"# This file is auto-generated.\n# Set this variable to true to refresh all dependencies.\nset (OBOS_REFRESH_DEPENDENCIES false)"
	)
endif()

add_subdirectory("src/oboskrnl")
add_subdirectory("src/isogen")