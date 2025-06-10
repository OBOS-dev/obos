/*
 * oboskrnl/mm/mm_sys.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>
#include <handle.h>

#include <mm/alloc.h>
#include <mm/context.h>
#include <mm/page.h>

struct vma_alloc_userspace_args
{
    prot_flags prot;
    vma_flags flags;
    handle file;
    uintptr_t offset;
};
void* Sys_VirtualMemoryAlloc(handle ctx, void* base, size_t size, struct vma_alloc_userspace_args* args, obos_status* status);
obos_status Sys_VirtualMemoryFree(handle ctx, void* base, size_t size);
obos_status Sys_VirtualMemoryProtect(handle ctx, void* base, size_t size, prot_flags newProt);
obos_status Sys_VirtualMemoryLock(handle ctx, void* base, size_t size);
obos_status Sys_VirtualMemoryUnlock(handle ctx, void* base, size_t size);

handle Sys_MakeNewContext(size_t ws_capacity);
obos_status Sys_ContextExpandWSCapacity(handle ctx, size_t ws_capacity);
obos_status Sys_ContextGetStat(handle ctx, memstat* stat);

size_t Sys_GetUsedPhysicalMemoryCount();

obos_status Sys_QueryPageInfo(handle ctx, void* base, page_info* info);

obos_status Sys_MakeDiskSwap(const char* path);
obos_status Sys_SwitchSwap(const char* path);