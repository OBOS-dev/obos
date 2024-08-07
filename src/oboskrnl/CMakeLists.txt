# oboskrnl/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

list (APPEND oboskrnl_sources 
	"klog.c" "locks/spinlock.c" "irq/irql.c" "scheduler/schedule.c"
	"scheduler/thread.c" "mm/bare_map.c" "allocators/basic_allocator.c"
	"text.c" "sanitizers/stack.c" "irq/irq.c" "scheduler/process.c"
	"irq/timer.c" "mm/context.c" "mm/init.c" "mm/swap.c"
	"mm/handler.c" "mm/alloc.c" "driver_interface/loader.c" "utils/hashmap.c"
	"driver_interface/pnp.c" "scheduler/dpc.c"
)

add_executable(oboskrnl)

set_source_files_properties("sanitizers/ubsan.c" PROPERTIES COMPILE_OPTIONS "-fvisibility=default")

target_compile_options(oboskrnl
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden>
)

target_link_libraries(oboskrnl PRIVATE gcc PRIVATE uacpi)

set_property(TARGET oboskrnl PROPERTY link_depends ${LINKER_SCRIPT})

target_include_directories(oboskrnl 
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_link_options(oboskrnl
	PRIVATE "-nostdlib"
#	PRIVATE "-fvisibility=hidden"
	PRIVATE "-T" PRIVATE ${LINKER_SCRIPT}
	PRIVATE ${TARGET_LINKER_OPTIONS}
)

target_compile_definitions(oboskrnl
	PRIVATE FIXEDPT_BITS=64
	PRIVATE OBOS_KERNEL=1
	PRIVATE FIXEDPT_WBITS=32)

target_sources(oboskrnl PRIVATE ${oboskrnl_sources})
