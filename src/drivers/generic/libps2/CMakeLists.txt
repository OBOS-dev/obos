# drivers/generic/libps2/CMakeLists.txt

# Copyright (c) 2025 Omar Berrow

# The motive behind having this instead of directly intergrating PS/2 drivers
# into a file is to allow PS/2 keyboard/mouse drivers to be shared between 
# platforms with different controllers.

add_executable(libps2 "keyboard.c" "detect.c" "main.c" "mouse.c")

target_compile_options(libps2
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_DRIVER_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fPIC>
)

set_property(TARGET libps2 PROPERTY link_depends ${DRIVER_LINKER_SCRIPT})

target_include_directories(libps2
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE "${CMAKE_SOURCE_DIR}/src/drivers"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_link_options(libps2
	PRIVATE "-nostdlib"
	PRIVATE "-fPIC"
	PRIVATE "-Wl,-shared"
#	PRIVATE "-Wl,--allow-shlib-undefined"
    PRIVATE "-T" PRIVATE ${DRIVER_LINKER_SCRIPT}
	PRIVATE ${TARGET_DRIVER_LINKER_OPTIONS}
)
target_compile_definitions(libps2 PRIVATE OBOS_DRIVER=1)

target_compile_definitions(libps2
	PRIVATE $<$<CONFIG:Debug>:OBOS_DEBUG>
	PRIVATE $<$<CONFIG:Release>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:RelWithDebInfo>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:MinSizeRel>:OBOS_RELEASE>
    PUBLIC OBOS_DRIVER=1
)
