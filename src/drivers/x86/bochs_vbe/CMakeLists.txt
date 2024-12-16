# drivers/x86/bochs_vbe/CMakeLists.txt
#
# Copyright (c) 2024 Omar Berrow

add_executable(bochs_vbe "main.c" "suspend.c" "io.c")

target_compile_options(bochs_vbe
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_DRIVER_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fPIC>
)

set_property(TARGET bochs_vbe PROPERTY link_depends ${DRIVER_LINKER_SCRIPT})

target_include_directories(bochs_vbe
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_link_options(bochs_vbe
	PRIVATE "-nostdlib"
	PRIVATE "-fPIC"
	PRIVATE "-Wl,-shared"
#	PRIVATE "-Wl,--allow-shlib-undefined"
    PRIVATE "-T" PRIVATE ${DRIVER_LINKER_SCRIPT}
	PRIVATE ${TARGET_DRIVER_LINKER_OPTIONS}
)
target_compile_definitions(bochs_vbe PRIVATE OBOS_DRIVER=1)
