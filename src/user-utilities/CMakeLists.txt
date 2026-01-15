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

if (NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86-64")
    add_compile_options("-fPIE")
    add_link_options("-pie")
endif()

add_executable (powerctl "powerctl.c")
add_executable (klog-level "klog-level.c")
add_executable (fork-test "fork-test.c")
add_executable (mem-usage "mem-usage.c")
add_executable (mount "mount.c")
add_executable (umount "umount.c")
add_executable (swapon "swapon.c")
add_executable (mkswap "mkswap.c")
add_executable (sync-anon "sync-anon.c")
add_executable (lsmod "lsmod.c")
add_executable (ldmod "ldmod.c")
add_executable (stmod "stmod.c")
add_executable (rmmod "rmmod.c")
add_executable (login "login.c")
add_executable (obos-ifconfig "obos-ifconfig.c")
target_link_libraries(login crypt)
install(TARGETS powerctl klog-level fork-test mem-usage mount umount swapon mkswap sync-anon)
install(TARGETS obos-ifconfig lsmod ldmod stmod rmmod login DESTINATION sbin)

if (${OBOS_ARCHITECTURE} STREQUAL "x86_64")
    add_executable(obos-gettime "x86_obos-gettime.c")
    add_executable(enable-kgdbstub "x86_enable-kgdbstub.c")
    install(TARGETS obos-gettime)
    install(TARGETS enable-kgdbstub DESTINATION sbin)
endif()
