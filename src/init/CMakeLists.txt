# init/CMakeLists.txt
#
# Copyright (c) 2025 Omar Berrow

if (${OBOS_ARCHITECTURE} STREQUAL "x86_64")
    add_library(syscall_invoke STATIC "x86_64-syscall.S")
endif()

if (OBOS_ENABLE_UBSAN)
    add_compile_options("-fno-sanitize=undefined")
endif()
if (OBOS_ENABLE_KASAN)
    add_compile_options("-fno-sanitize=address")
endif()

link_libraries(syscall_invoke)

add_executable (init "main.c" "motd.c")
