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

struct allocation_hdr {
    size_t sz;
};

void* malloc(size_t sz)
{
    sz += sizeof(struct allocation_hdr);
    struct allocation_hdr* hdr = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sz, nullptr);
    hdr->sz = sz;
    return hdr+1;
}
void* realloc(void* buf, size_t sz)
{
    struct allocation_hdr* hdr = buf;
    hdr--;
    hdr->sz += sz;
    return OBOS_KernelAllocator->Reallocate(OBOS_KernelAllocator, buf, sz, hdr->sz-sz, nullptr);
}
void free(void* buf)
{
    struct allocation_hdr* hdr = buf;
    hdr--;
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, hdr, hdr->sz);
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
    set_status(OBOS_STATUS_SUCCESS);
    const ustar_hdr* hdr = (ustar_hdr*)OBOS_InitrdBinary;
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
            return hdr;
        size_t filesize = oct2bin(hdr->filesize, uacpi_strnlen(hdr->filesize, 12));
        size_t filesize_rounded = (filesize + 0x1ff) & ~0x1ff;
        hdr = (ustar_hdr*)(((uintptr_t)hdr) + filesize_rounded + 512);
    }
    return nullptr;
}
