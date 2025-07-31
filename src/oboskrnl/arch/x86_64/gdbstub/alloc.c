/*
 * oboskrnl/arch/x86_64/gdbstub/alloc.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>

#include <allocators/base.h>

struct cmd_allocation_header
{
    size_t alloc_size;
};

void* Kdbg_Malloc(size_t sz)
{
    struct cmd_allocation_header* blk = nullptr;
    blk = OBOS_NonPagedPoolAllocator->Allocate(OBOS_NonPagedPoolAllocator, sz+sizeof(*blk), nullptr);
    blk->alloc_size = sz;
    return blk + 1;
}

void* Kdbg_Calloc(size_t nobj, size_t szobj)
{
    size_t sz = nobj * szobj;
    return memzero(Kdbg_Malloc(sz), sz);
}

void Kdbg_Free(void* buf)
{
    struct cmd_allocation_header* hdr = buf;
    hdr--;
    OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, hdr, hdr->alloc_size+sizeof(*hdr));
}

void* Kdbg_Realloc(void* buf, size_t newsize)
{
    if (!buf)
        return Kdbg_Malloc(newsize);
    if (!newsize)
    {
        Kdbg_Free(buf);
        return nullptr;
    }
    struct cmd_allocation_header* hdr = buf;
    hdr--;
    size_t oldsz = hdr->alloc_size;
    hdr->alloc_size = newsize;
    hdr = OBOS_NonPagedPoolAllocator->Reallocate(OBOS_NonPagedPoolAllocator, hdr, sizeof(*hdr)+newsize, sizeof(*hdr)+oldsz, nullptr);
    return hdr + 1;
}