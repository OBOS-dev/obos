# uACPI/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

include (${uacpi_cmake_file})

uacpi_add_sources()

add_library(uacpi STATIC ${UACPI_SOURCES} "kernel_api.c")

target_compile_options(uacpi
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${TARGET_COMPILE_OPTIONS_CPP}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-fno-stack-protector>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-fno-stack-check>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-fstack-protector-strong>
# PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-nostdlib>
# We don't enable -Werror in case future compiler versions introduce new warnings that the kernel triggers.
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-Wno-missing-field-initializers>
	PRIVATE $<$<CONFIG:Debug>:-g>
)

target_include_directories(uacpi PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl")
target_include_directories(uacpi PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})
target_include_directories(uacpi PUBLIC "${CMAKE_SOURCE_DIR}/src/uACPI")

target_link_libraries(uacpi PRIVATE ${LIBGCC})

target_compile_definitions(uacpi
	PRIVATE $<$<CONFIG:Debug>:OBOS_DEBUG>
	PRIVATE $<$<CONFIG:Release>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:RelWithDebInfo>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:MinSizeRel>:OBOS_RELEASE>
	PUBLIC UACPI_OVERRIDE_LIBC=1
	PUBLIC UACPI_OVERRIDE_ARCH_HELPERS=1
    PUBLIC UACPI_FORMATTED_LOGGING=1
	)
target_link_options(uacpi
	PRIVATE "-nostdlib"
)

if (DEFINED OBOS_ENABLE_KASAN)
	target_compile_options(uacpi PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-fsanitize=kernel-address>)
	set (OBOS_ENABLE_KASAN "1")
else()
	set (OBOS_ENABLE_KASAN "0")
endif()
target_compile_definitions(uacpi PRIVATE OBOS_KASAN_ENABLED=${OBOS_ENABLE_KASAN})