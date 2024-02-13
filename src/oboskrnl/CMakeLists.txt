# oboskrnl/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

set (oboskrnl_sources ${oboskrnl_sources}
	"console.cpp" "locks/spinlock.cpp" "klog.cpp")

add_executable(oboskrnl ${oboskrnl_sources} ${obos})

set_target_properties(oboskrnl PROPERTIES CXX_STANDARD 20)
set_target_properties(oboskrnl PROPERTIES CXX_STANDARD_REQUIRED 20)

set_target_properties(oboskrnl PROPERTIES LINK_DEPENDS "${LINKER_SCRIPT}")
set_target_properties(oboskrnl PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

target_include_directories(oboskrnl PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl")
target_include_directories(oboskrnl PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_compile_options(oboskrnl
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${TARGET_COMPILE_OPTIONS_CPP}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-fno-stack-protector -fno-stack-check -fno-lto>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-nostdlib>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-pedantic>
	PRIVATE $<$<CONFIG:Debug>:-g>
)
target_compile_definitions(oboskrnl
	PRIVATE $<$<CONFIG:Debug>:OBOS_DEBUG>
	PRIVATE $<$<CONFIG:Release>:OBOS_RELEASE>)
target_link_options(oboskrnl
	PUBLIC "-T" PUBLIC "${LINKER_SCRIPT}"
	PUBLIC "-nostdlib"
	PUBLIC ${TARGET_LINKER_OPTIONS}
)