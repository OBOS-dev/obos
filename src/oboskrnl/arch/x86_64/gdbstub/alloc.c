/*
 * oboskrnl/arch/x86_64/gdbstub/alloc.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>

#include <allocators/base.h>

void* Kdbg_Malloc(size_t sz)
{
    return OBOS_NonPagedPoolAllocator->Allocate(OBOS_NonPagedPoolAllocator, sz, nullptr);
}
void* Kdbg_Calloc(size_t nObjs, size_t szObj)
{
    return OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, nObjs, szObj, nullptr);
}
void* Kdbg_Realloc(void* ptr, size_t newSz)
{
    return OBOS_NonPagedPoolAllocator->Reallocate(OBOS_NonPagedPoolAllocator, ptr, newSz, nullptr);
}
void Kdbg_Free(void* ptr)
{
    size_t sz = 0;
    OBOS_NonPagedPoolAllocator->QueryBlockSize(OBOS_NonPagedPoolAllocator, ptr, &sz);
    OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, ptr, sz);
}