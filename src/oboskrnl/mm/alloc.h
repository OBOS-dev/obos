/*
 * oboskrnl/mm/alloc.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <mm/prot.h>
#include <mm/context.h>

// TODO: Make interface.
// TODO: Implement.

typedef enum vma_flags
{
    VMA_FLAGS_HUGE_PAGE = BIT(0),
    VMA_FLAGS_KERNEL_STACK = BIT(1),
    VMA_FLAGS_GUARD_PAGE = BIT(2),
    VMA_FLAGS_32BIT = BIT(3),
    VMA_FLAGS_HINT = BIT(4),
    VMA_FLAGS_NON_PAGED = BIT(5),
} vma_flags;

void* MmH_FindAvaliableAddress(context* ctx, size_t size, vma_flags flags, obos_status* status);
void* Mm_AllocateVirtualMemory(context* ctx, void* base, size_t size, prot_flags prot, vma_flags flags, obos_status* status);