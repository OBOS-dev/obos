# oboskrnl/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

list (APPEND oboskrnl_sources 
	"klog.c" "locks/spinlock.c" "irq/irql.c" "scheduler/schedule.c"
	"scheduler/thread.c" "mm/bare_map.c" "allocators/basic_allocator.c"
	"text.c" "sanitizers/stack.c" "irq/irq.c" "scheduler/process.c"
	"irq/timer.c" "mm/context.c" "mm/init.c" "mm/swap.c"
	"mm/handler.c" "mm/alloc.c"
	
)

add_executable(oboskrnl)

target_compile_options(oboskrnl
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
)

target_link_libraries(oboskrnl PRIVATE gcc PRIVATE uacpi)

set_property(TARGET oboskrnl PROPERTY link_depends ${LINKER_SCRIPT})

target_include_directories(oboskrnl 
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_link_options(oboskrnl
	PRIVATE "-nostdlib"
	PRIVATE "-T" PRIVATE ${LINKER_SCRIPT}
	PRIVATE ${TARGET_LINKER_OPTIONS}
)

if (DEFINED OBOS_ENABLE_UBSAN)
	list (APPEND oboskrnl_sources "sanitizers/ubsan.c")
	target_compile_options(oboskrnl
		PRIVATE $<$<COMPILE_LANGUAGE:C>:-fsanitize=undefined>
	)
	set(OBOS_ENABLE_UBSAN "1")
else()
	set(OBOS_ENABLE_UBSAN "0")
endif()
target_compile_definitions(oboskrnl PRIVATE OBOS_UBSAN_ENABLED=${OBOS_ENABLE_UBSAN})
if (DEFINED OBOS_ENABLE_KASAN)
	list (APPEND oboskrnl_sources "sanitizers/asan.c")
	target_compile_options(oboskrnl
		PRIVATE $<$<COMPILE_LANGUAGE:C>:-fsanitize=kernel-address>
	)
	set(OBOS_ENABLE_KASAN "1")
else()
	set(OBOS_ENABLE_KASAN "0")
endif()
target_compile_definitions(oboskrnl PRIVATE OBOS_KASAN_ENABLED=${OBOS_ENABLE_KASAN})

target_compile_definitions(oboskrnl
	PRIVATE $<$<CONFIG:Debug>:OBOS_DEBUG>
	PRIVATE $<$<CONFIG:Release>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:RelWithDebInfo>:OBOS_RELEASE>
	PRIVATE $<$<CONFIG:MinSizeRel>:OBOS_RELEASE>
	PRIVATE FIXEDPT_BITS=64
	PRIVATE FIXEDPT_WBITS=32)

target_sources(oboskrnl PRIVATE ${oboskrnl_sources})