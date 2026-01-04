/*
 * drivers/generic/extfs/dirent.c
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * Abandon all hope, ye who enter here.
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <allocators/base.h>

#include <mm/page.h>

#include <mm/swap.h>

#include "structs.h"

uint32_t ext_get_block_at_index(ext_cache* cache, uint32_t ino, struct inode_offset_location loc)
{
    ext_inode* inode = ext_read_inode(cache, ino);
    OBOS_ASSERT(inode);
    uint32_t block = 0;
#define idx_blocks 0
#define idx_indirect_block 1
#define idx_doubly_indirect_block 2
#define idx_triply_indirect_block 3
    uint8_t what =
        (loc.idx[3] == UINT32_MAX ? 
            (loc.idx[2] == UINT32_MAX ? 
                (loc.idx[1] != UINT32_MAX ? 
                    idx_indirect_block 
                        : idx_blocks)  // loc.idx[1] != UINT32_MAX
            : idx_doubly_indirect_block) // loc.idx[2] != UINT32_MAX 
        : idx_triply_indirect_block); // loc.idx[3] != UINT32_MAX
    switch (what) {
        case idx_blocks:
            block = inode->direct_blocks[loc.idx[0]];
            break;
        case idx_indirect_block:
        {
            if (!inode->indirect_block)
            {
                block = 0;
                break;
            }
            page* pg2 = nullptr;
            uint32_t* indirect_block = ext_read_block(cache, inode->indirect_block, &pg2);
            MmH_RefPage(pg2);
            block = indirect_block[loc.idx[1]];
            MmH_DerefPage(pg2);
            break;
        }
        case idx_doubly_indirect_block:
        {
            if (!inode->doubly_indirect_block)
            {
                block = 0;
                break;
            }
            page* pg2 = nullptr;
            uint32_t* doubly_indirect_block = ext_read_block(cache, inode->doubly_indirect_block, &pg2);
            MmH_RefPage(pg2);
            if (!doubly_indirect_block[loc.idx[2]])
            {
                block = 0;
                MmH_DerefPage(pg2);
                break;
            }
            uint32_t indirect_block_number = doubly_indirect_block[loc.idx[2]];
            MmH_DerefPage(pg2);
            uint32_t* indirect_block = ext_read_block(cache, indirect_block_number, &pg2);
            MmH_RefPage(pg2);
            block = indirect_block[loc.idx[1]];
            MmH_DerefPage(pg2);
            break;
        }
        case idx_triply_indirect_block:
        {
            if (!inode->triply_indirect_block)
            {
                block = 0;
                break;
            }
            page* pg2 = nullptr;
            uint32_t* triply_indirect_block = ext_read_block(cache, inode->triply_indirect_block, &pg2);
            MmH_RefPage(pg2);
            if (!triply_indirect_block[loc.idx[3]])
            {
                block = 0;
                MmH_DerefPage(pg2);
                break;
            }
            uint32_t doubly_indirect_block_number = triply_indirect_block[loc.idx[2]];
            MmH_DerefPage(pg2);
            uint32_t* doubly_indirect_block = ext_read_block(cache, doubly_indirect_block_number, &pg2);
            MmH_RefPage(pg2);
            if (!doubly_indirect_block[loc.idx[2]])
            {
                block = 0;
                MmH_DerefPage(pg2);
                break;
            }
            uint32_t indirect_block_number = doubly_indirect_block[loc.idx[2]];
            MmH_DerefPage(pg2);
            uint32_t* indirect_block = ext_read_block(cache, indirect_block_number, &pg2);
            MmH_RefPage(pg2);
            block = indirect_block[loc.idx[1]];
            MmH_DerefPage(pg2);
            break;
        }
    }
    Free(EXT_Allocator, inode, sizeof(*inode));
    return block;
}
#undef idx_blocks
#undef idx_indirect_block
#undef idx_doubly_indirect_block
#undef idx_triply_indirect_block

ext_dirent_cache* ext_dirent_populate(ext_cache* cache, uint32_t ino, const char* parent_name, bool recurse_directories, ext_dirent_cache* parent)
{
    if (!cache || !ino)
        return nullptr;

    if (strlen(parent_name) > 255)
        return nullptr;

    page* pg = nullptr;
    ext_inode* inode = ext_read_inode_pg(cache, ino, &pg);
    if (!inode)
        return nullptr;
    MmH_RefPage(pg);
    if (!ext_ino_test_type(inode, EXT2_S_IFDIR))
    {
        MmH_DerefPage(pg);
        return nullptr;
    }

    if (!parent)
    {
        parent = ZeroAllocate(EXT_Allocator, 1, sizeof(ext_dirent_cache) + strlen(parent_name), nullptr);
        parent->ent.ino = ino;
        parent->inode = inode;
        MmH_RefPage(pg);
        parent->pg = pg;
        parent->ent.name_len = strlen(parent_name);
        memcpy(parent->ent.name, parent_name, parent->ent.name_len);
    }
    else if (parent->populated)
    {
        MmH_DerefPage(pg);
        return parent;
    }

    size_t nToRead = le32_to_host(inode->blocks) * 512;
    uint8_t* buffer = Allocate(EXT_Allocator, nToRead, nullptr);
    ext_ino_read_blocks(cache, ino, 0, nToRead, buffer, nullptr);
    ext_dirent* ent = nullptr;
    for (size_t offset = 0; offset < nToRead; )
    {
        ent = (void*)(buffer+offset);
        if (!ent->ino)
            goto down;
        // if (strcmp(ent->name, ".") || strcmp(ent->name, ".."))
        //     goto down;

        if (!recurse_directories)
        {
            ext_dirent_cache* ent_cache = ZeroAllocate(EXT_Allocator, 1, sizeof(ext_dirent_cache), nullptr);
            ent_cache->ent = *ent;
            ent_cache->cache = cache;
            ent_cache->ent_block = ext_get_block_at_index(cache, parent->ent.ino, ext_get_blk_index_from_offset(cache, offset));
            ent_cache->ent_offset = offset % cache->block_size;
            ent_cache->rel_offset = offset;
            ent_cache->inode = ext_read_inode_pg(cache, ent->ino, &ent_cache->pg);
            memcpy(ent_cache->ent.name, ent->name, ent->name_len);
            ext_dirent_adopt(parent, ent_cache);
        }
        else
        {
            bool is_directory = false;
            if (cache->revision == 1)
                is_directory = ent->file_type == EXT2_FT_DIR;
            else
            {
                ext_inode* child_inode = ext_read_inode(cache, ent->ino);
                is_directory = ext_ino_test_type(inode, EXT2_S_IFDIR);
                Free(EXT_Allocator, child_inode, sizeof(ext_inode));
            }

            ext_dirent_cache* ent_cache = nullptr;
        
            if (is_directory)
                ent_cache = ext_dirent_populate(cache, ent->ino, ent->name, true, nullptr);
            else
            {
                ent_cache = ZeroAllocate(EXT_Allocator, 1, sizeof(ext_dirent_cache), nullptr);
                ent_cache->ent = *ent;
                ent_cache->inode = ext_read_inode_pg(cache, ent->ino, &ent_cache->pg);
                MmH_RefPage(ent_cache->pg);
                memcpy(ent_cache->ent.name, ent->name, ent->name_len);
            }

            ent_cache->ent.file_type = ent->file_type;
            ent_cache->ent.name_len = ent->name_len;
            ent_cache->ent.rec_len = ent->rec_len;
            ent_cache->ent_block = ext_get_block_at_index(cache, parent->ent.ino, ext_get_blk_index_from_offset(cache, offset));
            ent_cache->ent_offset = offset % cache->block_size;
            ent_cache->rel_offset = offset;
            ent_cache->cache = cache;

            ext_dirent_adopt(parent, ent_cache);
        }

        down:
        offset += ent->rec_len;
        if (!ent->rec_len)
        {
            MmH_DerefPage(pg);
            OBOS_Error("extfs: %s: directory corrupted, returning nullptr\n", __func__);
            return nullptr;
        }
    }

    parent->populated = true;
    MmH_DerefPage(pg);
    return parent;
}

// Adapated from vfs/dirent.c

static size_t str_search(const char* str, char ch)
{
    size_t ret = strchr(str, ch);
    for (; str[ret] == ch && str[ret] != 0; ret++)
        ;
    return ret;
}
static ext_dirent_cache* on_match(ext_dirent_cache** const curr_, ext_dirent_cache** const root, const char** const tok, size_t* const tok_len, const char** const path, 
                        size_t* const path_len)
{
    ext_dirent_cache *curr = *curr_;
    ext_dirent_populate(curr->cache, curr->ent.ino, curr->ent.name, false, curr->parent);
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
ext_dirent_cache* ext_dirent_lookup_from(const char* path, ext_dirent_cache* root)
{
    if (!path)
        return nullptr;
    size_t path_len = strlen(path);
    if (!path_len)
        return root;
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
        ext_dirent_cache* curr = root;
        {
            string name = {};
            name.cap = 33;
            name.ls = root->ent.name;
            name.len = root->ent.name_len;
            if (OBOS_CompareStringNC(&name, tok, tok_len))
            {
                root = curr->children.head;
                continue;
            }
        }
        for (curr = root->children.head; curr;)
        {
            string name = {};
            name.cap = 33;
            name.ls = curr->ent.name;
            name.len = curr->ent.name_len;
            if (OBOS_CompareStringNC(&name, tok, tok_len))
            {
                // Match!
                ext_dirent_cache* what = 
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

void ext_dirent_flush(ext_cache* cache, ext_dirent_cache* ent)
{
    if (!cache || !ent)
        return;
    page* pg = nullptr;
    void* block = ext_read_block(cache, ent->ent_block, &pg);
    MmH_RefPage(pg);
    memcpy((char*)block + ent->ent_offset, &ent->ent, sizeof(ent->ent));
    Mm_MarkAsDirtyPhys(pg);
    MmH_DerefPage(pg);
}