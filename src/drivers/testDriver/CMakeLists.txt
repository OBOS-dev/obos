# drivers/testDriver/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

add_executable(testDriver "main.cpp")

set_target_properties(testDriver PROPERTIES CXX_STANDARD 20)
set_target_properties(testDriver PROPERTIES CXX_STANDARD_REQUIRED 20)

set_target_properties(testDriver PROPERTIES LINK_DEPENDS "${LINKER_SCRIPT}")

target_include_directories(testDriver PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl")
target_include_directories(testDriver PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_compile_definitions(testDriver PRIVATE OBOS_DRIVER=1)

target_compile_options(testDriver
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${TARGET_DRIVER_COMPILE_OPTIONS_CPP}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_DRIVER_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-fno-stack-protector>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-fno-stack-check>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-fstack-protector-strong>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-Wstack-usage=65536>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-Wno-missing-field-initializers>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-fPIE>
	PRIVATE $<$<CONFIG:Debug>:-g>
)
target_compile_definitions(testDriver
	PRIVATE $<$<CONFIG:Debug>:OBOS_DEBUG>
	PRIVATE $<$<CONFIG:Release>:OBOS_RELEASE>)
target_link_options(testDriver
	PUBLIC "-nostdlib"
	PUBLIC "-pie"
	PUBLIC ${TARGET_DRIVER_LINKER_OPTIONS}
)

target_link_libraries(testDriver PRIVATE ${LIBGCC})