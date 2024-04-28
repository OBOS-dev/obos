# CMakeLists.txt
#
# Copyright (c) 2024 Omar Berrow

cmake_minimum_required(VERSION 3.7)

project(OBOS C CXX ASM_NASM)

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
	if (OBOS_ARCHITECTURE STREQUAL "x86_64")
		message("Fetching Limine.")
		include("dependencies/limine.cmake")
		if (DEFINED OBOS_ENABLE_KDBG AND DEFINED OBOS_KDBG_ENABLE_ZYDIS)
			message("Fetching ZyDis.")
            include("dependencies/zydis.cmake")
		endif()
	endif()
	message("Fetching uACPI.")
	include ("dependencies/uacpi.cmake")
	file (WRITE "${CMAKE_SOURCE_DIR}/dependencies/needs_download.cmake"
		"# This file is auto-generated.\n# Set this variable to true or delete this file to refresh all dependencies.\nset (OBOS_REFRESH_DEPENDENCIES false)"
	)
else()
	if (OBOS_ARCHITECTURE STREQUAL "x86_64")
		if (NOT EXISTS limine_install)
			include("dependencies/limine.cmake")
		endif()
		if (DEFINED OBOS_ENABLE_KDBG AND DEFINED OBOS_KDBG_ENABLE_ZYDIS AND NOT EXISTS zydis_c)
			include("dependencies/zydis.cmake")
		endif()
	endif()
	if (NOT EXISTS uacpi_cmake_file)
        include("dependencies/uacpi.cmake")
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

if (OBOS_ARCHITECTURE_ENDIANNESS STREQUAL "Little-Endian")
	add_compile_definitions("OBOS_ARCHITECTURE_ENDIANNESS=obos::arch::endianness::LittleEndian")
elseif (OBOS_ARCHITECTURE_ENDIANNESS STREQUAL "Big-Endian")
	add_compile_definitions("OBOS_ARCHITECTURE_ENDIANNESS=obos::arch::endianness::BigEndian")
else()
	add_compile_definitions("OBOS_ARCHITECTURE_ENDIANNESS=obos::arch::endianness::MixedEndian")
endif()

add_subdirectory("src/libs/uACPI")
add_subdirectory("src/oboskrnl")
add_subdirectory("src/isogen")
add_subdirectory("src/drivers/testDriver")
