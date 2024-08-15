/*
 * oboskrnl/vfs/alloc.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "utils/tree.h"
#include <int.h>

#include <allocators/base.h>
#include <allocators/basic_allocator.h>

allocator_info* Vfs_Allocator;

static basic_allocator alloc;
void* Vfs_Malloc(size_t cnt)
{
    if (!Vfs_Allocator)
    {
        OBOSH_ConstructBasicAllocator(&alloc);
        Vfs_Allocator = (allocator_info*)&alloc;
    }
    return Vfs_Allocator->Allocate(Vfs_Allocator, cnt, nullptr);
}
void* Vfs_Calloc(size_t nObjs, size_t szObj)
{
    if (!Vfs_Allocator)
    {
        OBOSH_ConstructBasicAllocator(&alloc);
        Vfs_Allocator = (allocator_info*)&alloc;
    }
    return Vfs_Allocator->ZeroAllocate(Vfs_Allocator, nObjs, szObj, nullptr);
}
void* Vfs_Realloc(void* what, size_t cnt)
{
    if (!Vfs_Allocator)
        return nullptr;
    return Vfs_Allocator->Reallocate(Vfs_Allocator, what, cnt, nullptr);
}
void Vfs_Free(void* what)
{
    if (!Vfs_Allocator)
        return;
    size_t szBlk = 0;
    Vfs_Allocator->QueryBlockSize(Vfs_Allocator, what, &szBlk);
    Vfs_Allocator->Free(Vfs_Allocator, what, szBlk);
}