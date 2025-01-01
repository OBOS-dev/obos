# uACPI/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

add_library(sanitizers STATIC "asan.c" "stack.c" "ubsan.c" "prof.c" "asan_memory.c")

target_compile_options(sanitizers
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
#	PRIVATE $<$<COMPILE_LANGUAGE:C>:-g0>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=default> # We should have all symbols exported
)

target_include_directories(sanitizers PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl")
target_include_directories(sanitizers PRIVATE "${CMAKE_SOURCE_DIR}/src/sanitizers")
target_include_directories(sanitizers PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})
target_include_directories(sanitizers PUBLIC "${CMAKE_SOURCE_DIR}/src/uACPI")

target_link_libraries(sanitizers PRIVATE ${LIBGCC})

target_compile_definitions(sanitizers
	PRIVATE $<$<CONFIG:Debug>:OBOS_DEBUG>
	PRIVATE $<$<CONFIG:Release>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:RelWithDebInfo>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:MinSizeRel>:OBOS_RELEASE>
	PUBLIC UACPI_OVERRIDE_LIBC=1
	PUBLIC UACPI_OVERRIDE_ARCH_HELPERS=1
    PUBLIC UACPI_FORMATTED_LOGGING=1
    PUBLIC OBOS_KERNEL=1
    PUBLIC IS_UACPI_BUILD=1
	PRIVATE OBOS_ARCHITECTURE_HAS_ACPI=${OBOS_ARCHITECTURE_HAS_ACPI}
)
target_link_options(sanitizers
	PRIVATE "-nostdlib"
)
