/*
 * oboskrnl/vfs/alloc.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>

#include <vfs/alloc.h>

#include <allocators/base.h>
#include <allocators/basic_allocator.h>

allocator_info* Vfs_Allocator;

struct allocation_hdr {
    size_t sz;
};

static basic_allocator alloc;
void* Vfs_Malloc(size_t cnt)
{
    if (!Vfs_Allocator)
    {
        OBOSH_ConstructBasicAllocator(&alloc);
        Vfs_Allocator = (allocator_info*)&alloc;
    }
    cnt += sizeof(struct allocation_hdr);
    struct allocation_hdr *hdr = Vfs_Allocator->ZeroAllocate(Vfs_Allocator, 1, cnt, nullptr);
    hdr->sz = cnt-sizeof(struct allocation_hdr);
    return hdr+1;
}

void* Vfs_Calloc(size_t nObjs, size_t szObj)
{
    if (!Vfs_Allocator)
    {
        OBOSH_ConstructBasicAllocator(&alloc);
        Vfs_Allocator = (allocator_info*)&alloc;
    }
    return Vfs_Malloc(nObjs*szObj);
}

void* Vfs_Realloc(void* what, size_t cnt)
{
    if (!Vfs_Allocator || !what)
        return nullptr;
    struct allocation_hdr* hdr = what;
    hdr--;
    size_t old_size = hdr->sz+sizeof(*hdr);
    hdr->sz = cnt;
    hdr = Vfs_Allocator->Reallocate(Vfs_Allocator, hdr, cnt+sizeof(*hdr), old_size, nullptr);
    return hdr+1;
}

void Vfs_Free(void* what)
{
    if (!Vfs_Allocator || !what)
        return;
    struct allocation_hdr* hdr = what;
    hdr--;
    Vfs_Allocator->Free(Vfs_Allocator, hdr, hdr->sz + sizeof(*hdr));
}
