# CMakeLists.txt
#
# Copyright (c) 2024-2025 Omar Berrow

cmake_minimum_required(VERSION 3.10)

# Boolean options.

option (OBOS_UP "Make a Uniprocessor-Build of obos.")
option (OBOS_ENABLE_UBSAN "Enables UBSan.")
option (OBOS_ENABLE_KASAN "Enables KAsan.")
option (OBOS_ENABLE_FANALYZER "(Developers only) Enables the compiler's static analyzer")
option (OBOS_ENABLE_WERROR "(Developers only) Enables the -Werror option")
option (OBOS_USE_CLANG "Use clang instead of GCC to build.")
option (OBOS_NVS_SAVE_S3 "Enable compile-time support for saving NVS before S3" ON)
option (OBOS_ENABLE_LOCK_PROFILING "(Experimental) Enable profiling for locks.")
option (OBOS_ENABLE_PROFILER "(Experimental) Enable profiling.")
option (OBOS_LAZY_IRQL "Enables lazy IRQL. Recommended if the main target of the kernel is hypervisors, where writing to cr8 is slooow.")
option (OBOS_USE_OBOS_GCC "Uses arch-obos-gcc instead of obos-elf-gcc. Ignored if OBOS_USE_CLANG is true.")
option (OBOS_COMPILE_USER_EXAMPLES "Compiles user examples" ${OBOS_USE_OBOS_GCC})

if (NOT OBOS_USE_OBOS_GCC AND OBOS_COMPILE_USER_EXAMPLES)
	message(WARNING "OBOS_COMPILE_USER_EXAMPLES must be OFF if OBOS_USE_OBOS_GCC is OFF")
	set(OBOS_COMPILE_USER_EXAMPLES OFF)
endif()

# String options

# NOTE: Unless we add FORCE to the end, CMake shouldn't override user values.
set (OUTPUT_DIR ${CMAKE_SOURCE_DIR}/out CACHE STRING "The output directory.")
set (OBOS_PAGE_REPLACEMENT_ALGORITHM "Aging" CACHE STRING "(Kernel developers only) The page replacement algorithm used by the MM.")
set (OBOS_DEV_PREFIX "/dev" CACHE STRING "Directory where the kernel places devices.")
set (OBOS_CLANG_SUFFIX "" CACHE STRING "Some Linux distributions add a suffix to various clang and LLVM commands, such as the version used. Specify that suffix, if needed")

file (TO_CMAKE_PATH "${OUTPUT_DIR}" OUTPUT_DIR)
file (TO_NATIVE_PATH "${OUTPUT_DIR}" OUTPUT_DIR)

set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})

# set(CMAKE_CXX_FLAGS_RELEASE "-O3" CACHE STRING "Flags used by the CXX compiler during RELEASE builds.")
# set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g" CACHE STRING "Flags used by the CXX compiler during RELWITHDEBINFO builds.")
# set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os" CACHE STRING "Flags used by the CXX compiler during MINSIZEREL builds.")

project(
	OBOS
	LANGUAGES C CXX ASM
	DESCRIPTION "Hybrid Kernel with advanced driver loading."
	HOMEPAGE_URL https://github.com/OBOS-dev/obos
	)

if (OBOS_ARCHITECTURE STREQUAL "x86_64")
	enable_language(ASM_NASM)
else()
	enable_language(ASM-ATT)
endif()

if (OBOS_USE_CLANG)
	add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-Wno-error=unknown-pragmas>
						$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unknown-pragmas>
						$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-error=unknown-attributes>
						$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-error=c23-extensions>)
	if (OBOS_ENABLE_KASAN)
		set (OBOS_ENABLE_KASAN OFF)
		message (WARNING "OBOS does not support Clang's ASAN implementation. If you really care about this, make a PR, as I don't.")
	endif()
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

if (OBOS_UP)
	add_compile_definitions(OBOS_UP=1)
endif()

if (OBOS_NVS_SAVE_S3)
	set (OBOS_NVS_SAVE_S3 "1")
else()
	set (OBOS_NVS_SAVE_S3 "0")
endif()
add_compile_definitions(OBOS_NVS_SAVE_S3=${OBOS_NVS_SAVE_S3})

if (OBOS_LAZY_IRQL)
	set (OBOS_LAZY_IRQL "1")
else()
	set (OBOS_LAZY_IRQL "0")
endif()
add_compile_definitions(OBOS_LAZY_IRQL=${OBOS_LAZY_IRQL})

if (OBOS_ENABLE_LOCK_PROFILING)
	set (OBOS_ENABLE_LOCK_PROFILING "1")
else()
	set (OBOS_ENABLE_LOCK_PROFILING "0")
endif()
add_compile_definitions(OBOS_ENABLE_LOCK_PROFILING=${OBOS_ENABLE_LOCK_PROFILING})

if (OBOS_ENABLE_PROFILER)
	set (OBOS_ENABLE_PROFILER "1")
else()
	set (OBOS_ENABLE_PROFILER "0")
endif()
add_compile_definitions(OBOS_ENABLE_PROFILING=${OBOS_ENABLE_PROFILER})

add_compile_definitions(OBOS_BINARY_DIRECTORY="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

add_compile_definitions(OBOS_ARCHITECTURE_BITS=${OBOS_ARCHITECTURE_BITS})

add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-fno-strict-aliasing>)

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

#list (APPEND oboskrnl_sources "sanitizers/ubsan.c")
if (OBOS_ENABLE_UBSAN)
	add_compile_options(
		$<$<COMPILE_LANGUAGE:C>:-fsanitize=undefined>
	)
	set (OBOS_ENABLE_UBSAN "1")
else()
	set (OBOS_ENABLE_UBSAN "0")
endif()
add_compile_definitions(OBOS_UBSAN_ENABLED=${OBOS_ENABLE_UBSAN})

#list (APPEND oboskrnl_sources "sanitizers/asan.c")
if (OBOS_ENABLE_KASAN)
	add_compile_options(
		$<$<COMPILE_LANGUAGE:C>:-fsanitize=kernel-address>
	)
	set (OBOS_ENABLE_KASAN "1")
else()
	set (OBOS_ENABLE_KASAN "0")
endif()
add_compile_definitions(OBOS_KASAN_ENABLED=${OBOS_ENABLE_KASAN})
add_compile_definitions(OBOS_IRQL_COUNT=${OBOS_IRQL_COUNT})

add_compile_definitions(
	$<$<CONFIG:Debug>:OBOS_DEBUG>
	$<$<CONFIG:Release>:OBOS_RELEASE>
	$<$<CONFIG:RelWithDebInfo>:OBOS_RELEASE>
	$<$<CONFIG:MinSizeRel>:OBOS_RELEASE>
)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(OBOS_DEBUG "1")
endif()
if (CMAKE_BUILD_TYPE STREQUAL "Release" OR
	CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR
	CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
	set(OBOS_RELEASE "1")
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

if (OBOS_ENABLE_WERROR AND NOT OBOS_ENABLE_FANALYZER)
	add_compile_options("-Werror" $<$<COMPILE_LANGUAGE:C>:-Wno-error=unused-function> $<$<COMPILE_LANGUAGE:C>:-Wno-error=unused-parameter>)
endif()
if (OBOS_ENABLE_FANALYZER)
	add_compile_options($<$<COMPILE_LANGUAGE:C>:-fanalyzer>)
endif()
if (OBOS_ENABLE_FANALYZER AND OBOS_ENABLE_WERROR)
	message(WARNING "OBOS_ENABLE_FANALYZER and OBOS_ENABLE_WERROR are mutually exclusive. Preferring OBOS_ENABLE_FANALYZER.")
endif()

# Now done somewhere else.
# if (NOT DEFINED OBOS_DEV_PREFIX)
# 	set(OBOS_DEV_PREFIX "/dev/")
# endif()
configure_file(${CMAKE_SOURCE_DIR}/src/inc/dev_prefix.h.in ${CMAKE_SOURCE_DIR}/src/oboskrnl/inc/dev_prefix.h @ONLY)

add_compile_definitions(
	OBOS_ARCHITECTURE_HAS_ACPI=${OBOS_ARCHITECTURE_HAS_ACPI}
	OBOS_ARCHITECTURE_HAS_PCI=${OBOS_ARCHITECTURE_HAS_PCI}
)

# Include the kernel
add_subdirectory("src/oboskrnl")
if (OBOS_ARCHITECTURE STREQUAL "m68k")
	add_subdirectory("src/oboskrnl/arch/m68k/loader")
endif()
add_subdirectory("src/uACPI")
add_subdirectory("src/sanitizers")
# Include drivers
get_target_property(uacpi_INCLUDES uacpi INCLUDE_DIRECTORIES)
include_directories(${uacpi_INCLUDES})
if (OBOS_ARCHITECTURE STREQUAL "x86_64")
	add_subdirectory("src/drivers/x86/bochs_vbe")
	add_subdirectory("src/drivers/x86/uart")
endif()
add_subdirectory("src/drivers/generic/initrd")
add_subdirectory("src/drivers/generic/slowfat")
add_subdirectory("src/drivers/test_driver")
add_subdirectory("src/drivers/generic/ahci")
add_subdirectory("src/drivers/generic/r8169")
add_subdirectory("src/isogen")

add_subdirectory("src/init")
if (OBOS_COMPILE_USER_EXAMPLES)
	add_subdirectory("src/user_examples")
endif()