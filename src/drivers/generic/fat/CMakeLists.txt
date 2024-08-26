# drivers/generic/fat/CMakeLists.txt
# 
# Copyright (c) 2024 Omar Berrow

add_executable(fat "main.c")

target_compile_options(fat
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_DRIVER_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fPIC>
)

set_property(TARGET fat PROPERTY link_depends ${DRIVER_LINKER_SCRIPT})

target_include_directories(fat 
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_link_options(fat
	PRIVATE "-nostdlib"
	PRIVATE "-Wl,-pic"
	PRIVATE "-T${DRIVER_LINKER_SCRIPT}"
	PRIVATE "-Wl,-shared"
#	PRIVATE "-Wl,--allow-shlib-undefined"
	PRIVATE ${TARGET_DRIVER_LINKER_OPTIONS}
)
target_compile_definitions(fat PRIVATE OBOS_DRIVER=1)