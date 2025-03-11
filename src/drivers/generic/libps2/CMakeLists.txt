# drivers/generic/libps2/CMakeLists.txt

# Copyright (c) 2025 Omar Berrow

# The motive behind having this instead of directly intergrating PS/2 drivers
# into a file is to allow PS/2 keyboard/mouse drivers to be shared between 
# platforms with different controllers.

add_library(ps2 STATIC "keyboard.c")

target_compile_options(ps2
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_DRIVER_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fPIC>
#	PRIVATE $<$<COMPILE_LANGUAGE:C>:-g0>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=default> # libps2 should have all its symbols exported
)

target_include_directories(ps2 
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE "${CMAKE_SOURCE_DIR}/src/drivers"
	PRIVATE "${CMAKE_SOURCE_DIR}/src/sanitizers"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES}
)

target_compile_definitions(ps2
	PRIVATE $<$<CONFIG:Debug>:OBOS_DEBUG>
	PRIVATE $<$<CONFIG:Release>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:RelWithDebInfo>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:MinSizeRel>:OBOS_RELEASE>
    PUBLIC OBOS_DRIVER=1
)

target_link_options(ps2
	PRIVATE "-nostdlib"
)
