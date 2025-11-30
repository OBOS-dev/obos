# drivers/generic/freebsd-e1000/CMakeLists.txt
#
# Copyright (c) 2025 Omar Berrow

add_executable(e1000 "main.c" "io.c" ${E1000_SOURCES})

target_compile_options(e1000
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_DRIVER_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fPIC>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-sanitize=kernel-address>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-error>
)

if (OBOS_ENABLE_FANALYZER)
	target_compile_options(e1000 PRIVATE "-fno-analyzer")
endif()

set_property(TARGET e1000 PROPERTY link_depends ${DRIVER_LINKER_SCRIPT})

target_include_directories(e1000
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE "${CMAKE_SOURCE_DIR}/src/drivers/generic/freebsd-e1000"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_link_options(e1000
	PRIVATE "-nostdlib"
	PRIVATE "-fPIC"
	PRIVATE "-Wl,-shared"
#	PRIVATE "-Wl,--allow-shlib-undefined"
    PRIVATE "-T" PRIVATE ${DRIVER_LINKER_SCRIPT}
	PRIVATE ${TARGET_DRIVER_LINKER_OPTIONS}
)
target_compile_definitions(e1000 PRIVATE OBOS_DRIVER=1)
