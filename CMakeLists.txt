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
	message("Fetching Hyper.")
	include("dependencies/hyper.cmake")
	message("Fetching uACPI.")
	include ("dependencies/uacpi.cmake")
	file (WRITE "${CMAKE_SOURCE_DIR}/dependencies/needs_download.cmake"
		"# This file is auto-generated.\n# Set this variable to true or delete this file to refresh all dependencies.\nset (OBOS_REFRESH_DEPENDENCIES false)"
	)
else()
	if (NOT EXISTS hyper_install)
		include("dependencies/hyper.cmake")
	endif()
	if (NOT EXISTS uacpi_cmake_file)
        include("dependencies/uacpi.cmake")
	endif()
endif()

if (NOT DEFINED OUTPUT_DIR)
	set (OUTPUT_DIR ${CMAKE_SOURCE_DIR}/out)
endif()

if (DEFINED OBOS_UP)
	add_compile_definitions(OBOS_UP=1)
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
add_compile_definitions (OBOS_ARCHITECTURE_ENDIANNESS="${OBOS_ARCHITECTURE_ENDIANNESS}")

if (DEFINED OBOS_ENABLE_UBSAN)
	list (APPEND oboskrnl_sources "sanitizers/ubsan.c")
	add_compile_options(
		$<$<COMPILE_LANGUAGE:C>:-fsanitize=undefined>
	)
	set(OBOS_ENABLE_UBSAN "1")
else()
	set(OBOS_ENABLE_UBSAN "0")
endif()
add_compile_definitions(OBOS_UBSAN_ENABLED=${OBOS_ENABLE_UBSAN})
if (DEFINED OBOS_ENABLE_KASAN)
	list (APPEND oboskrnl_sources "sanitizers/asan.c")
	add_compile_options(
		$<$<COMPILE_LANGUAGE:C>:-fsanitize=kernel-address>
	)
	set(OBOS_ENABLE_KASAN "1")
else()
	set(OBOS_ENABLE_KASAN "0")
endif()
add_compile_definitions(OBOS_KASAN_ENABLED=${OBOS_ENABLE_KASAN})

add_compile_definitions(
	$<$<CONFIG:Debug>:OBOS_DEBUG>
	$<$<CONFIG:Release>:OBOS_RELEASE>
	$<$<CONFIG:RelWithDebInfo>:OBOS_RELEASE>
	$<$<CONFIG:MinSizeRel>:OBOS_RELEASE>
)

# Include the kernel
add_subdirectory("src/oboskrnl")
add_subdirectory("src/uACPI")
# Include drivers
if (OBOS_ARCHITECTURE STREQUAL "x86_64")
	add_subdirectory("src/drivers/x86_64/uart")
endif()
add_subdirectory("src/isogen")

set_property(GLOBAL PROPERTY C_STANDARD 17
				    PROPERTY C_STANDARD_REQUIRED true)