# drivers/test_driver/CMakeLists.txt
# 
# Copyright (c) 2024 Omar Berrow

add_executable(test_driver "main.c" "fireworks.c" "rand.c")

target_compile_options(test_driver
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_DRIVER_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fPIC>
)
# drivers/x86/uart/CMakeLists.txt
#
# Copyright (c) 2024 Omar Berrow

add_executable(uart "main.c" "serial_port.c")

target_compile_options(uart
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_DRIVER_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fPIC>
)

set_property(TARGET uart PROPERTY link_depends ${DRIVER_LINKER_SCRIPT})

target_include_directories(uart
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_link_options(uart
	PRIVATE "-nostdlib"
	PRIVATE "-fPIC"
	PRIVATE "-Wl,-shared"
#	PRIVATE "-Wl,--allow-shlib-undefined"
    PRIVATE "-T" PRIVATE ${DRIVER_LINKER_SCRIPT}
	PRIVATE ${TARGET_DRIVER_LINKER_OPTIONS}
)
target_compile_definitions(uart PRIVATE OBOS_DRIVER=1)

set_target_properties(test_driver PROPERTIES link_depends ${DRIVER_LINKER_SCRIPT})

target_include_directories(test_driver 
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_link_options(test_driver
	PRIVATE "-nostdlib"
	PRIVATE "-fPIC"
	PRIVATE "-T${DRIVER_LINKER_SCRIPT}"
	PRIVATE "-Wl,-shared"
#	PRIVATE "-Wl,--allow-shlib-undefined"
	PRIVATE ${TARGET_DRIVER_LINKER_OPTIONS}
)

target_compile_definitions(test_driver PRIVATE OBOS_DRIVER=1)
