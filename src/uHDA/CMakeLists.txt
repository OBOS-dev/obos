# uHDA/CMakeLists.txt

# Copyright (c) 2025 Omar Berrow

include (${uhda_cmake_file})

add_library(uhda STATIC ${UHDA_SOURCES} "kernel_api.c")

target_compile_options(uhda
	PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:${TARGET_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-ffreestanding>
	# PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	# PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
#	PRIVATE $<$<COMPILE_LANGUAGE:C>:-g0>
	PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-fvisibility=default> # uhda should have all its symbols exported
	# PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++20>
)

set_property(TARGET uhda PROPERTY CXX_STANDARD 20)
set_property(TARGET uhda PROPERTY CXX_STANDARD_REQUIRED ON)

if (OBOS_ENABLE_PROFILER)
	target_compile_options(uhda PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-finstrument-functions>)
endif()

# All symbols exported, except for the kernel api of course.
set_source_files_properties("kernel_api.c" PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")

target_include_directories(uhda PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl")
target_include_directories(uhda PRIVATE "${CMAKE_SOURCE_DIR}/src/sanitizers")
target_include_directories(uhda PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_link_libraries(uhda PRIVATE ${LIBGCC} PRIVATE sanitizers)

target_compile_definitions(uhda
	PRIVATE $<$<CONFIG:Debug>:OBOS_DEBUG>
	PRIVATE $<$<CONFIG:Release>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:RelWithDebInfo>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:MinSizeRel>:OBOS_RELEASE>
    PUBLIC OBOS_KERNEL=1
    PUBLIC IS_UHDA_BUILD=1
)
target_link_options(uhda
	PRIVATE "-nostdlib"
)
