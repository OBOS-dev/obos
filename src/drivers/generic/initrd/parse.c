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
    struct allocation_hdr* hdr = ZeroAllocate(OBOS_KernelAllocator, 1, sz, nullptr);
    hdr->sz = sz;
    return hdr+1;
}
void* realloc(void* buf, size_t sz)
{
    struct allocation_hdr* hdr = buf;
    hdr--;
    hdr->sz += sz;
    return Reallocate(OBOS_KernelAllocator, buf, sz, hdr->sz-sz, nullptr);
}
void free(void* buf)
{
    struct allocation_hdr* hdr = buf;
    hdr--;
    Free(OBOS_KernelAllocator, hdr, hdr->sz);
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


// Adapated from vfs/dirent.c

static size_t str_search(const char* str, char ch)
{
    size_t ret = strchr(str, ch);
    for (; str[ret] == ch && str[ret] != 0; ret++)
        ;
    return ret;
}
static initrd_inode* on_match(initrd_inode** const curr_, initrd_inode** const root, const char** const tok, size_t* const tok_len, const char** const path, 
                        size_t* const path_len)
{
    initrd_inode *curr = *curr_;
    *root = curr;
    const char *newtok = (*tok) + str_search(*tok, '/');
    if (newtok >= (*path + *path_len))
        return curr;
    if (!curr->children.nChildren)
        return nullptr; // could not find node.
    *tok = newtok;
    size_t currentPathLen = strlen(*tok)-1;
    if ((*tok)[currentPathLen] != '/')
        currentPathLen++;
    while ((*tok)[currentPathLen] == '/')
        currentPathLen--;
    *tok_len = strchr(*tok, '/');
    if (*tok_len != currentPathLen)
        (*tok_len)--;
    while ((*tok)[(*tok_len) - 1] == '/')
        (*tok_len)--;
    return nullptr;
}
initrd_inode* DirentLookupFrom(const char* path, initrd_inode* root)
{
    if (!path)
        return nullptr;
    size_t path_len = strlen(path);
    if (!path_len)
        return nullptr;
    for (; *path == '/'; path++, path_len--)
        ;
    const char* tok = path;
    size_t tok_len = strchr(tok, '/');
    if (tok_len != path_len)
        tok_len--;
    while (tok[tok_len - 1] == '/')
        tok_len--;
    if (!tok_len)
        return nullptr;
    while(root)
    {
        initrd_inode* curr = root;
        if (strcmp(root->name, tok))
        {
            // Match!
            root = curr->children.head ? curr->children.head : root;
            initrd_inode* what = 
                on_match(&curr, &root, &tok, &tok_len, &path, &path_len);
            if (what)
                return what;
            continue;
        }
        for (curr = root->children.head; curr;)
        {
            if (strcmp(curr->name, tok))
            {
                // Match!
                initrd_inode* what = 
                    on_match(&curr, &root, &tok, &tok_len, &path, &path_len);
                if (what)
                    return what;
                curr = curr->children.head ? curr->children.head : curr;
                break;
            }

            // root = curr->children.head ? curr->children.head : root;
            curr = curr->next;
        }
        if (!curr)
            root = root->parent;
    }
    return nullptr;
}