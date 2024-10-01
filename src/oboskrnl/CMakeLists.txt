# oboskrnl/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

list (APPEND oboskrnl_sources 
	"klog.c" "locks/spinlock.c" "irq/irql.c" "scheduler/schedule.c"
	"scheduler/thread.c" "mm/bare_map.c" "allocators/basic_allocator.c"
	"text.c" "sanitizers/stack.c" "irq/irq.c" "scheduler/process.c"
	"irq/timer.c" "mm/context.c" "mm/init.c" "mm/swap.c"
	"mm/handler.c" "mm/alloc.c" "driver_interface/loader.c" "utils/hashmap.c"
	"driver_interface/pnp.c" "irq/dpc.c" "locks/mutex.c" "locks/semaphore.c"
	"locks/event.c" "locks/wait.c" "cmdline.c" "vfs/init.c" 
	"vfs/alloc.c" "utils/string.c" "vfs/mount.c" "vfs/dirent.c"
	"vfs/fd.c" "vfs/pagecache.c" "vfs/async.c" "mm/pmm.c"
	"driver_interface/pci_irq.c" "mbr.c" "gpt.c" "partition.c"
	"utils/uuid.c" "mm/disk_swap.c" "memmanip.c" "signal.c"
	"mm/aging.c" "mm/initial_swap.c"
)

add_executable(oboskrnl)

set_source_files_properties("sanitizers/ubsan.c" PROPERTIES COMPILE_OPTIONS "-fvisibility=default")
set_source_files_properties("klog.c" PROPERTIES COMPILE_OPTIONS "-Wno-error")
set_source_files_properties("vfs/mount.c" PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")

target_compile_options(oboskrnl
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector-all>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-builtin-memset>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden>
	# https://discord.com/channels/440442961147199490/1141057599584878645/1269032825579901069
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Werror=incompatible-pointer-types>
)

target_link_libraries(oboskrnl
	PRIVATE ${LIBGCC} 
	PRIVATE uacpi
)

set_property(TARGET oboskrnl PROPERTY link_depends ${LINKER_SCRIPT})

target_include_directories(oboskrnl 
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE "${CMAKE_SOURCE_DIR}/src/drivers"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES}
)

target_link_options(oboskrnl
	PRIVATE "-nostdlib"
#	PRIVATE "-fvisibility=hidden"
	PRIVATE "-T" PRIVATE ${LINKER_SCRIPT}
	PRIVATE ${TARGET_LINKER_OPTIONS}
)

target_compile_definitions(oboskrnl
	PRIVATE FIXEDPT_BITS=${OBOS_ARCHITECTURE_BITS}
	PRIVATE OBOS_KERNEL=1
	PRIVATE "FIXEDPT_WBITS=(${OBOS_ARCHITECTURE_BITS}-8)"
)

target_sources(oboskrnl PRIVATE ${oboskrnl_sources})

set_source_files_properties(
    "arch/m68k/entry.c"
    PROPERTIES
    OBJECT_DEPENDS "${OUTPUT_DIR}/initrd"
)