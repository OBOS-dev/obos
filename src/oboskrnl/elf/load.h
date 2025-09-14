/*
 * oboskrnl/elf/load.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>

#include <mm/context.h>

typedef struct elf_info {
    void* base;
    void* rtld_base;
    uintptr_t entry;
    uintptr_t real_entry;
} elf_info;

// if dryRun is true, nothing is mapped, but checks are still made on the ELF.
// noLdr should always be false unless you know what you're doing.
obos_status OBOS_LoadELF(context* ctx, const void* file, size_t szFile, elf_info* info, bool dryRun, bool noLdr);
