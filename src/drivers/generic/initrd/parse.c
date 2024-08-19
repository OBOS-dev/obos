/*
 * drivers/generic/initrd/parse.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <cmdline.h>

#include <stdint.h>
#include <uacpi_libc.h>

#include "parse.h"
#include "ustar_hdr.h"

#include <allocators/base.h>

#include <utils/hashmap.h>

static struct hashmap* cache;

static uint64_t hash(const void *item, uint64_t seed0, uint64_t seed1)
{
    const ustar_hdr* pck = item;
    return hashmap_sip(pck->filename, uacpi_strnlen(pck->filename, 100), seed0, seed1);
}
static int cmp(const void *a, const void *b, void *udata)
{
    OBOS_UNUSED(udata);
    const ustar_hdr* pck1 = a;
    const ustar_hdr* pck2 = b;
    return uacpi_strncmp(pck1->filename, pck2->filename, 100);
}
void* malloc(size_t sz)
{
    return OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, sz, nullptr);
}
void* realloc(void* buf, size_t sz)
{
    return OBOS_KernelAllocator->Reallocate(OBOS_KernelAllocator, buf, sz, nullptr);
}
void free(void* buf)
{
    size_t blkSize = 0;
    OBOS_KernelAllocator->QueryBlockSize(OBOS_KernelAllocator, buf, &blkSize);
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, buf, blkSize);
}
static void initialize_hashmap()
{
    cache = hashmap_new_with_allocator(
        malloc, realloc, free,
        sizeof(ustar_hdr), 
        64, 0, 0, 
        hash, cmp, 
        nullptr, 
        nullptr);
}

#define set_status(to) status ? (*status = to) : (void)0
const ustar_hdr* GetFile(const char* path, obos_status* status)
{
    if (!OBOS_InitrdBinary)
    {
        set_status(OBOS_STATUS_NOT_FOUND);
        return nullptr;
    }
    if (strlen(path) > 100)
    {
        set_status(OBOS_STATUS_INVALID_ARGUMENT);
        return nullptr;
    }
    if (!cache)
        initialize_hashmap();
    set_status(OBOS_STATUS_SUCCESS);
    ustar_hdr what = {};
    memcpy(what.filename, path, uacpi_strnlen(path, 100));
    const ustar_hdr* hdr = (ustar_hdr*)hashmap_get(cache, &what);
    if (hdr)
        return hdr;
    hdr = (ustar_hdr*)OBOS_InitrdBinary;
    size_t pathlen = uacpi_strnlen(path, 100);
    char path_slash[100];
    memcpy(path_slash, path, pathlen);
    if (path_slash[pathlen-1] != '/')
    {
        path_slash[pathlen] = '/';
        path_slash[++pathlen] = '\0';
    }
    while (memcmp(hdr->magic, USTAR_MAGIC, 6))
    {
        if (uacpi_strncmp(path_slash, hdr->filename, pathlen) == 0 ||
            uacpi_strncmp(path, hdr->filename, pathlen) == 0
        )
        {
            hashmap_set(cache, hdr);
            return hdr;
        }
        size_t filesize = oct2bin(hdr->filesize, uacpi_strnlen(hdr->filesize, 12));
        size_t filesize_rounded = (filesize + 0x1ff) & ~0x1ff;
        hdr = (ustar_hdr*)(((uintptr_t)hdr) + filesize_rounded + 512);
    }
    return nullptr;
}
void FreeCache()
{
    hashmap_free(cache);
}