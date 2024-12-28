# init/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

add_executable(init "main.c")

target_compile_definitions(init
	PRIVATE $<$<CONFIG:Debug>:OBOS_DEBUG>
	PRIVATE $<$<CONFIG:Release>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:RelWithDebInfo>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:MinSizeRel>:OBOS_RELEASE>
)

target_compile_options(init
    PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
    PRIVATE $<$<COMPILE_LANGUAGE:C>:-fPIE>
)

target_link_options(init PRIVATE "-nostdlib" PRIVATE "-fPIE")
