# user_examples/CMakeLists.txt
#
# Copyright (c) 2025 Omar Berrow

if (OBOS_ENABLE_UBSAN)
    add_compile_options("-fno-sanitize=undefined")
endif()
if (OBOS_ENABLE_KASAN)
    add_compile_options("-fno-sanitize=address")
endif()

link_libraries(syscall_invoke)

add_executable (powerctl "powerctl_example.c")
add_executable (klog-level "klog_level_example.c")
add_executable (fork-test "fork_example.c")
add_executable (mem-usage "mem_usage_example.c")
add_executable (mount "mount_example.c")
add_executable (umount "umount_example.c")
add_executable (swapon "swapon_example.c")
add_executable (mkswap "mkswap_example.c")
add_executable (sync-anon "sync_anon_example.c")
add_executable (lsmod "lsmod_example.c")
add_executable (ldmod "ldmod_example.c")
add_executable (stmod "stmod_example.c")
add_executable (rmmod "rmmod_example.c")

if (${OBOS_ARCHITECTURE} STREQUAL "x86_64")
    add_executable(obos-gettime "x86_timeofday_example.c")
endif()
