# drivers/x86/i8042/CMakeLists.txt
# 
# Copyright (c) 2025 Omar Berrow

add_executable(i8042 "main.c" "ctlr.c")

target_compile_options(i8042
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_DRIVER_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fPIC>
)

set_property(TARGET i8042 PROPERTY link_depends ${DRIVER_LINKER_SCRIPT})

target_include_directories(i8042 
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE "${CMAKE_SOURCE_DIR}/src/drivers"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_link_options(i8042
	PRIVATE "-nostdlib"
	PRIVATE "-fPIC"
	PRIVATE "-Wl,-shared"
#	PRIVATE "-Wl,--allow-shlib-undefined"
    PRIVATE "-T" PRIVATE ${DRIVER_LINKER_SCRIPT}
	PRIVATE ${TARGET_DRIVER_LINKER_OPTIONS}
)
target_compile_definitions(i8042 PRIVATE OBOS_DRIVER=1)

target_link_libraries(i8042 PRIVATE ps2)
