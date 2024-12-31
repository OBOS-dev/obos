# drivers/generic/slowfat/CMakeLists.txt
# 
# Copyright (c) 2024 Omar Berrow

add_executable(slowfat "main.c" "probe.c" "lookup.c" "interface.c"
				   "cls_alloc.c" "io.c" "create.c"
)

target_compile_options(slowfat
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_DRIVER_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fPIC>
)

set_property(TARGET slowfat PROPERTY link_depends ${DRIVER_LINKER_SCRIPT})

target_include_directories(slowfat 
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_link_options(slowfat
	PRIVATE "-nostdlib"
	PRIVATE "-fPIC"
	PRIVATE "-T${DRIVER_LINKER_SCRIPT}"
	PRIVATE "-Wl,-shared"
#	PRIVATE "-Wl,--allow-shlib-undefined"
	PRIVATE ${TARGET_DRIVER_LINKER_OPTIONS}
)
target_compile_definitions(slowfat PRIVATE OBOS_DRIVER=1)
