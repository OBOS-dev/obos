# oboskrnl/CMakeLists.txt

# Copyright (c) 2024-2025 Omar Berrow

list (APPEND oboskrnl_sources 
	"klog.c" "locks/spinlock.c" "irq/irql.c" "scheduler/schedule.c"
	"scheduler/thread.c" "mm/bare_map.c" "allocators/basic_allocator.c"
	"text.c" "irq/irq.c" "scheduler/process.c" "power/shutdown.c"
	"irq/timer.c" "mm/context.c" "mm/init.c" "mm/swap.c"
	"mm/handler.c" "mm/alloc.c" "driver_interface/loader.c" "power/event.c"
	"driver_interface/pnp.c" "irq/dpc.c" "locks/mutex.c" "locks/semaphore.c"
	"locks/event.c" "locks/wait.c" "cmdline.c" "vfs/init.c" 
	"vfs/alloc.c" "utils/string.c" "vfs/mount.c" "vfs/dirent.c"
	"vfs/fd.c" "vfs/dummy_devices.c" "vfs/tty.c" "mm/pmm.c"
	"driver_interface/pci_irq.c" "mbr.c" "gpt.c" "partition.c"
	"utils/uuid.c" "mm/disk_swap.c" "memmanip.c" "signal.c"
	"mm/aging.c" "mm/initial_swap.c" "locks/pushlock.c" "vfs/pipe.c"
	"syscall.c" "handle.c" "scheduler/sched_sys.c" "mm/mm_sys.c"
	"mm/fork.c" "power/suspend.c" "power/device.c" "power/init.c"
	"driver_interface/pci.c" "utils/shared_ptr.c" "locks/sys_futex.c" "vfs/fd_sys.c"
	"elf/load.c" "driver_interface/drv_sys.c" "sig_sys.c" "execve.c"
	"init_proc.c" "vfs/create.c" "net/route.c" "net/eth.c"
	"net/ip.c" "net/arp.c" "power/event.c" "net/udp.c"
	"net/icmp.c" "net/tcp.c" "vfs/socket.c" "net/lo.c"
	"kinit.c" "vfs/local_socket.c" "perm.c" "contrib/tjec.c"
	"contrib/csprng.c" "vfs/pty.c"
)

add_executable(oboskrnl)

set_source_files_properties("sanitizers/ubsan.c" PROPERTIES COMPILE_OPTIONS "-fvisibility=default")
set_source_files_properties("klog.c" PROPERTIES COMPILE_OPTIONS "-Wno-error")
set_source_files_properties("vfs/mount.c" PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")
set_source_files_properties("${CMAKE_SOURCE_DIR}/dependencies/flanterm/src/flanterm_backends/fb.c" PROPERTIES COMPILE_OPTIONS "-fno-sanitize=kernel-address")
set_source_files_properties("${CMAKE_SOURCE_DIR}/dependencies/flanterm/src/flanterm.c" PROPERTIES COMPILE_OPTIONS "-fno-sanitize=kernel-address")

target_compile_options(oboskrnl
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fstack-protector>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-pie>
	# https://discord.com/channels/440442961147199490/1141057599584878645/1269032825579901069
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Werror=incompatible-pointer-types>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-error=address>
)

if (OBOS_ENABLE_PROFILER)
	target_compile_options(oboskrnl PRIVATE $<$<COMPILE_LANGUAGE:C>:-finstrument-functions>)
endif()

target_link_libraries(oboskrnl
	PRIVATE ${LIBGCC} 
	PRIVATE sanitizers
)
if (OBOS_ARCHITECTURE_HAS_ACPI)
	target_link_libraries(oboskrnl
		PRIVATE uacpi
	)
endif()

set_property(TARGET oboskrnl PROPERTY link_depends ${LINKER_SCRIPT})

if (DEFINED OBOS_RELEASE)
	set_property(TARGET oboskrnl PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

target_include_directories(oboskrnl 
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE "${CMAKE_SOURCE_DIR}/src/drivers"
	PRIVATE "${CMAKE_SOURCE_DIR}/src/sanitizers"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES}
)

target_link_options(oboskrnl
	PRIVATE "-nostdlib"
	PRIVATE "-fno-pie"
	PRIVATE "-Wl,--export-dynamic"
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

if (OBOS_ENABLE_UHDA)
	target_link_libraries(oboskrnl PRIVATE uhda)
endif()

include("${CMAKE_SOURCE_DIR}/src/isogen/isogen.cmake")

include(GNUInstallDirs)

install(FILES ${CMAKE_SOURCE_DIR}/src/oboskrnl/vfs/keycode.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/obos/)
install(FILES ${CMAKE_SOURCE_DIR}/src/oboskrnl/vfs/mouse.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/obos/)