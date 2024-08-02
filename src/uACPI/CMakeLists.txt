# uACPI/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

include (${uacpi_cmake_file})

uacpi_add_sources()

add_library(uacpi STATIC ${UACPI_SOURCES} "kernel_api.c")

target_compile_options(uacpi
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=default> # uACPI should have all its symbols exported
)

# Except for the kernel api of course.
set_source_files_properties("kernel_api.c" PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")

target_include_directories(uacpi PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl")
target_include_directories(uacpi PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})
target_include_directories(uacpi PUBLIC "${CMAKE_SOURCE_DIR}/src/uACPI")

target_link_libraries(uacpi PRIVATE gcc
)

target_compile_definitions(uacpi
	PRIVATE $<$<CONFIG:Debug>:OBOS_DEBUG>
	PRIVATE $<$<CONFIG:Release>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:RelWithDebInfo>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:MinSizeRel>:OBOS_RELEASE>
	PUBLIC UACPI_OVERRIDE_LIBC=1
	PUBLIC UACPI_OVERRIDE_ARCH_HELPERS=1
    PUBLIC UACPI_FORMATTED_LOGGING=1
    PUBLIC OBOS_KERNEL=1
	)
target_link_options(uacpi
	PRIVATE "-nostdlib"
)