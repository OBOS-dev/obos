# CMakeLists.txt
#
# Copyright (c) 2024 Omar Berrow

if (NOT DEFINED OUTPUT_DIR)
	set (OUTPUT_DIR ${CMAKE_SOURCE_DIR}/out)
endif()

file (TO_CMAKE_PATH "${OUTPUT_DIR}" OUTPUT_DIR)
file (TO_NATIVE_PATH "${OUTPUT_DIR}" OUTPUT_DIR)

set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})

cmake_minimum_required(VERSION 3.7)

project(OBOS C CXX ASM)
if (OBOS_ARCHITECTURE STREQUAL "x86_64")
	enable_language(ASM_NASM)
else()
	enable_language(ASM-ATT)
endif()

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
		message("Fetching Hyper.")
		include("dependencies/hyper.cmake")
	endif()
	message("Fetching uACPI.")
	include ("dependencies/uacpi.cmake")
	file (WRITE "${CMAKE_SOURCE_DIR}/dependencies/needs_download.cmake"
		"# This file is auto-generated.\n# Set this variable to true or delete this file to refresh all dependencies.\nset (OBOS_REFRESH_DEPENDENCIES false)"
	)
else()
	if (NOT EXISTS hyper_install AND OBOS_ARCHITECTURE STREQUAL "x86_64")
		include("dependencies/hyper.cmake")
	endif()
	if (NOT EXISTS uacpi_cmake_file)
        include("dependencies/uacpi.cmake")
	endif()
endif()

if (DEFINED OBOS_UP)
	add_compile_definitions(OBOS_UP=1)
endif()

add_compile_definitions(OBOS_BINARY_DIRECTORY="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

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

list (APPEND oboskrnl_sources "sanitizers/ubsan.c")
if (DEFINED OBOS_ENABLE_UBSAN)
	add_compile_options(
		$<$<COMPILE_LANGUAGE:C>:-fsanitize=undefined>
	)
	set(OBOS_ENABLE_UBSAN "1")
else()
	set(OBOS_ENABLE_UBSAN "0")
endif()
add_compile_definitions(OBOS_UBSAN_ENABLED=${OBOS_ENABLE_UBSAN})
list (APPEND oboskrnl_sources "sanitizers/asan.c")
if (DEFINED OBOS_ENABLE_KASAN)
	add_compile_options(
		$<$<COMPILE_LANGUAGE:C>:-fsanitize=kernel-address>
	)
	set(OBOS_ENABLE_KASAN "1")
else()
	set(OBOS_ENABLE_KASAN "0")
endif()
add_compile_definitions(OBOS_KASAN_ENABLED=${OBOS_ENABLE_KASAN})
add_compile_definitions(OBOS_IRQL_COUNT=${OBOS_IRQL_COUNT})

add_compile_definitions(
	$<$<CONFIG:Debug>:OBOS_DEBUG>
	$<$<CONFIG:Release>:OBOS_RELEASE>
	$<$<CONFIG:RelWithDebInfo>:OBOS_RELEASE>
	$<$<CONFIG:MinSizeRel>:OBOS_RELEASE>
)

if (NOT DEFINED OBOS_PAGE_REPLACEMENT_ALGORITHM)
	set(OBOS_PAGE_REPLACEMENT_ALGORITHM "Aging")
endif()
string(TOUPPER "${OBOS_PAGE_REPLACEMENT_ALGORITHM}" OBOS_PAGE_REPLACEMENT_ALGORITHM)
set(OBOS_PAGE_REPLACEMENT_ALGORITHM_DEFINE "OBOS_PAGE_REPLACEMENT_")
string(APPEND OBOS_PAGE_REPLACEMENT_ALGORITHM_DEFINE "${OBOS_PAGE_REPLACEMENT_ALGORITHM}")
add_compile_definitions(${OBOS_PAGE_REPLACEMENT_ALGORITHM_DEFINE}=1)
string(APPEND OBOS_PAGE_REPLACEMENT_ALGORITHM "\"")
string(PREPEND OBOS_PAGE_REPLACEMENT_ALGORITHM "\"")
add_compile_definitions(OBOS_PAGE_REPLACEMENT_ALGORITHM=${OBOS_PAGE_REPLACEMENT_ALGORITHM})

set_property(GLOBAL PROPERTY C_STANDARD 17
					PROPERTY C_STANDARD_REQUIRED true
					PROPERTY C_EXTENSIONS On)

if (DEFINED OBOS_ENABLE_WERROR AND NOT DEFINED OBOS_ENABLE_FANALYZER)
	add_compile_options("-Werror" $<$<COMPILE_LANGUAGE:C>:-Wno-error=unused-function> $<$<COMPILE_LANGUAGE:C>:-Wno-error=unused-parameter>)
endif()
if (DEFINED OBOS_ENABLE_FANALYZER)
	add_compile_options($<$<COMPILE_LANGUAGE:C>:-fanalyzer>)
endif()
if (DEFINED OBOS_ENABLE_FANALYZER AND DEFINED OBOS_ENABLE_WERROR)
	message(WARNING "OBOS_ENABLE_FANALYZER and OBOS_ENABLE_WERROR are mutually exclusive. Preferring OBOS_ENABLE_FANALYZER.")
endif()

if (NOT DEFINED OBOS_DEV_PREFIX)
	set(OBOS_DEV_PREFIX "/dev/")
endif()
configure_file(${CMAKE_SOURCE_DIR}/src/inc/dev_prefix.h.in ${CMAKE_SOURCE_DIR}/src/oboskrnl/inc/dev_prefix.h @ONLY)

add_compile_definitions(
	OBOS_ARCHITECTURE_HAS_ACPI=${OBOS_ARCHITECTURE_HAS_ACPI}
	OBOS_ARCHITECTURE_HAS_PCI=${OBOS_ARCHITECTURE_HAS_PCI}
)

# Include the kernel
add_subdirectory("src/oboskrnl")
add_subdirectory("src/uACPI")
# Include drivers
get_target_property(uacpi_INCLUDES uacpi INCLUDE_DIRECTORIES)
include_directories(${uacpi_INCLUDES})
if (OBOS_ARCHITECTURE STREQUAL "x86_64")
	add_subdirectory("src/drivers/x86_64/uart")
	endif()
add_subdirectory("src/drivers/generic/initrd")
add_subdirectory("src/drivers/generic/ahci")
add_subdirectory("src/drivers/generic/slowfat")
add_subdirectory("src/drivers/test_driver")
add_subdirectory("src/isogen")
if (OBOS_ARCHITECTURE STREQUAL "m68k")
	add_subdirectory("src/oboskrnl/arch/m68k/loader")
endif()
