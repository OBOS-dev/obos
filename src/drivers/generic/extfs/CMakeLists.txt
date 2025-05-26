# drivers/generic/extfs/CMakeLists.txt
# 
# Copyright (c) 2025 Omar Berrow

add_executable(extfs "main.c" "probe.c")

target_compile_options(extfs
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_DRIVER_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fPIC>
)

set_property(TARGET extfs PROPERTY link_depends ${DRIVER_LINKER_SCRIPT})

target_include_directories(extfs 
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_link_options(extfs
	PRIVATE "-nostdlib"
	PRIVATE "-fPIC"
	PRIVATE "-T${DRIVER_LINKER_SCRIPT}"
	PRIVATE "-Wl,-shared"
#	PRIVATE "-Wl,--allow-shlib-undefined"
	PRIVATE ${TARGET_DRIVER_LINKER_OPTIONS}
)
target_compile_definitions(extfs PRIVATE OBOS_DRIVER=1)
