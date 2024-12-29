/*
 * init/allocator.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <stdint.h>
#include <stddef.h>

#include "allocator.h"

__attribute__((no_sanitize("address")))
static void *alloc_memzero(void* dest_, size_t sz)
{
    char* dest = dest_;
    for (size_t i = 0; i < sz; i++)
        dest[i] = 0;
    return dest_;
}

__attribute__((no_sanitize("address")))
static void *alloc_memcpy(void* dest_, const void* src_, size_t sz)
{
    char* dest = dest_;
    const char* src = src_;
    for (size_t i = 0; i < sz; i++)
        dest[i] = src[i];
    return dest_;
}

static int allocate_region(allocator* alloc, cache* c, size_t cache_index)
{
    size_t sz = 1 << (cache_index+4);
    size_t sz_node = sz;
    if (sz < init_pgsize())
        sz = init_pgsize();
    sz += sizeof(region);
    region* reg = init_mmap(sz);
    if (!reg)
        return 0;
    reg->start = reg+1;
    reg->sz = sz-sizeof(region);
    reg->magic = REGION_MAGIC;
    append_node(c->region_list, reg);
    if (sz_node == reg->sz)
        append_node(c->free, (freelist_node*)reg->start);
    else
    {
        for (size_t i = 0; i < (reg->sz/sz_node); i++)
        {
            freelist_node* node = (freelist_node*)(((uintptr_t)reg->start) + i*sz_node);
            append_node(c->free, node);
        }
    }
    return 1;
}

void lock(cache* c)
{
    (void)(c);
    return;
}
void unlock(cache* c)
{
    (void)(c);
    return;
}

void* init_malloc(allocator* alloc, size_t nBytes)
{
    if (nBytes <= 16)
        nBytes = 16;
    else
        nBytes = (size_t)1 << (64-__builtin_clzll(nBytes));
    if (nBytes > (4*1024*1024))
        return NULL; // invalid argument

    size_t cache_index = __builtin_ctzll(nBytes)-4;
    cache* c = &alloc->caches[cache_index];

    lock(c);

    void* ret = c->free.tail;
    if (!ret)
    {
        if (!allocate_region(alloc, c, cache_index))
        {
            unlock(c);
            return NULL; // OOM
        }

        ret = c->free.tail;
    }

    remove_node(c->free, c->free.tail);

    unlock(c);
    return ret;
}

void* init_calloc(allocator* alloc, size_t cnt, size_t nBytes)
{
    void* blk = init_malloc(alloc, cnt*nBytes);
    return blk ? alloc_memzero(blk, cnt*nBytes) : NULL;
}

void* init_realloc(allocator* alloc, void* blk, size_t new_size, size_t old_size)
{
    if (!blk)
        return init_malloc(alloc, new_size);
    if (!new_size)
    {
        init_free(alloc, blk, old_size);
        return NULL;
    }
    void* newblk = init_malloc(alloc, new_size);
    if (!blk)
        return blk;
    alloc_memcpy(newblk, blk, old_size);
    init_free(alloc, blk, old_size);
    return newblk;
}

void  init_free(allocator* alloc, void* blk, size_t nBytes)
{
    if (!blk)
        return;
    if (nBytes <= 16)
        nBytes = 16;
    else
        nBytes = (size_t)1 << (64-__builtin_clzll(nBytes));
    if (nBytes > (4*1024*1024))
        return; // invalid argument

    size_t cache_index = __builtin_ctzll(nBytes)-4;
    cache* c = &alloc->caches[cache_index];

    lock(c);

    alloc_memzero(blk, sizeof(freelist_node));
    append_node(c->free, (freelist_node*)blk);

    unlock(c);
}

allocator init_allocator;

void* malloc(size_t sz)
{
    return init_malloc(&init_allocator, sz);
}

void* calloc(size_t cnt, size_t sz)
{
    return init_calloc(&init_allocator, cnt, sz);
}

void* realloc(void* blk, size_t sz, size_t oldsz)
{
    return init_realloc(&init_allocator, blk, sz, oldsz);
}

void  free(void* blk, size_t sz)
{
    init_free(&init_allocator, blk, sz);
}
