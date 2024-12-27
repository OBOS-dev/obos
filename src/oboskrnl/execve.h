/*
 * oboskrnl/execve.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>
#include <handle.h>

#include <elf/load.h>

// envp can be nullptr
// argv can be nullptr
obos_status Sys_ExecVE(const void* buf, size_t szBuf, char* const* argv, char* const* envp);

struct exec_aux_values {
    elf_info elf;
    struct {
        void* ptr;
        size_t phnum;
        size_t phent;
    } phdr;

    // NOTE: Make sure to free the next fields after copying them to whereever the process expects them.
    // Guaranteed to be allocated with OBOS_KernelAllocator.

    char* const* argv;
    char* const* envp;
    size_t argc;
    size_t envpc;
};

OBOS_NORETURN OBOS_WEAK void OBOSS_HandControlTo(struct context* ctx, struct exec_aux_values* aux);
