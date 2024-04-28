# oboskrnl/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

set (oboskrnl_sources ${oboskrnl_sources}
	"console.cpp" "locks/spinlock.cpp" "klog.cpp" "irq/irql.cpp" 
	"vmm/pg_context.cpp" "vmm/map.cpp" "allocators/allocator.cpp" "allocators/basic_allocator.cpp"
	"vmm/init.cpp" "vmm/demand_paging.cpp" "vmm/mprot.cpp" "irq/irq.cpp"
	"scheduler/init.cpp" "scheduler/thread.cpp" "scheduler/scheduler.cpp" "kmain.cpp"
)

add_executable(oboskrnl)

set_target_properties(oboskrnl PROPERTIES CXX_STANDARD 20)
set_target_properties(oboskrnl PROPERTIES CXX_STANDARD_REQUIRED 20)

set_target_properties(oboskrnl PROPERTIES LINK_DEPENDS "${LINKER_SCRIPT}")

target_include_directories(oboskrnl PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl")
target_include_directories(oboskrnl PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_compile_options(oboskrnl
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${TARGET_COMPILE_OPTIONS_CPP}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-fno-stack-protector>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-fno-stack-check>
	# PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-flto>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-fstack-protector-strong>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-Wstack-usage=65536>
# PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-nostdlib>
# We don't enable -Werror in case future compiler versions introduce new warnings that the kernel triggers.
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-Wno-missing-field-initializers>
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

if (DEFINED OBOS_ENABLE_KASAN)
	target_compile_options(oboskrnl PRIVATE $<$<COMPILE_LANGUAGE:CXX,C>:-fsanitize=kernel-address>)
	list(APPEND oboskrnl_sources "sanitizers/asan.cpp")
	set (OBOS_ENABLE_KASAN "1")
else()
	set (OBOS_ENABLE_KASAN "0")
endif()
target_compile_definitions(oboskrnl PRIVATE OBOS_KASAN_ENABLED=${OBOS_ENABLE_KASAN})

target_link_libraries(oboskrnl PRIVATE uacpi PRIVATE ${LIBGCC})

if (DEFINED OBOS_SLOW_ARTHIMETRIC)
	target_compile_definitions(oboskrnl PUBLIC OBOS_SLOW_ARTHIMETRIC=1)
endif()
if (OBOS_ARCHITECTURE STREQUAL "x86_64")
	if (DEFINED OBOS_ENABLE_KDBG)
		target_compile_definitions(oboskrnl PUBLIC OBOS_KDBG_ENABLED=1 PUBLIC ZYAN_NO_LIBC=1)
		if (DEFINED OBOS_KDBG_ENABLE_ZYDIS)
			list (APPEND oboskrnl_sources ${zydis_c})
			target_compile_definitions(oboskrnl PUBLIC OBOS_KDBG_ENABLE_ZYDIS=1)
		else()
			target_compile_definitions(oboskrnl PUBLIC OBOS_KDBG_ENABLE_ZYDIS=0)
		endif()
	else()
		target_compile_definitions(oboskrnl PUBLIC OBOS_KDBG_ENABLED=0)
	endif()
endif()
if (OBOS_ARCHITECTURE_BITS EQUAL 64)
	list(APPEND oboskrnl_sources "driver_interface/loader64.cpp")
else()
	message(SEND_ERROR "Unimplemented: OBOS_ARCHITECTURE_BITS=32")
endif()

target_sources(oboskrnl PRIVATE ${oboskrnl_sources})