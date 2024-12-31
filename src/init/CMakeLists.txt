# init/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

add_executable(init "main.c" "allocator.c")

target_compile_definitions(init
	PRIVATE $<$<CONFIG:Debug>:OBOS_DEBUG>
	PRIVATE $<$<CONFIG:Release>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:RelWithDebInfo>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:MinSizeRel>:OBOS_RELEASE>
)

target_compile_options(init
    PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
    PRIVATE $<$<COMPILE_LANGUAGE:C>:-fPIE>
    PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-sanitize=undefined>
    PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-sanitize=kernel-address>
    PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
    PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
)

target_link_options(init PRIVATE "-nostdlib" PRIVATE "-fPIE")

if (OBOS_ARCHITECTURE STREQUAL "x86_64")
	target_compile_options(init PRIVATE $<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only>)
endif()
