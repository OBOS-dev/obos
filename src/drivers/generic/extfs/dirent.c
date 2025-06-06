/*
 * drivers/generic/extfs/dirent.c
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * Abandon all hope, ye who enter here.
 */

#include <int.h>
#include <error.h>
#include <memmanip.h>

#include <allocators/base.h>

#include <mm/page.h>

#include "structs.h"

ext_dirent_cache* ext_dirent_populate(ext_cache* cache, uint32_t ino, const char* parent_name, bool recurse_directories)
{
    if (!cache || !ino)
        return nullptr;

    if (strlen(parent_name) > 255)
        return nullptr;

    page* pg = nullptr;
    ext_inode* inode = ext_read_inode_pg(cache, ino, &pg);
    MmH_RefPage(pg);
    if (!inode)
        return nullptr;
    if (!ext_ino_test_type(inode, EXT2_S_IFDIR))
    {
        MmH_DerefPage(pg);
        return nullptr;
    }

    ext_dirent_cache* parent = ZeroAllocate(EXT_Allocator, 1, sizeof(ext_dirent_cache) + strlen(parent_name), nullptr);
    parent->ent.ino = ino;
    parent->inode = inode;
    MmH_RefPage(pg);
    parent->pg = pg;
    parent->ent.name_len = strlen(parent_name);
    memcpy(parent->ent.name, parent_name, parent->ent.name_len);

    size_t nToRead = le32_to_host(inode->blocks) * 512;
    uint8_t* buffer = Allocate(EXT_Allocator, nToRead, nullptr);
    ext_ino_read_blocks(cache, ino, 0, nToRead, buffer, nullptr);
    ext_dirent* ent = nullptr;
    for (size_t offset = 0; offset < nToRead; )
    {
        ent = (void*)(buffer+offset);
        if (!ent->ino)
            goto down;
        if (strcmp(ent->name, ".") || strcmp(ent->name, ".."))
            goto down;

        if (!recurse_directories)
        {
            ext_dirent_cache* ent_cache = ZeroAllocate(EXT_Allocator, 1, sizeof(ext_dirent_cache) + ent->name_len, nullptr);
            ent_cache->ent = *ent;
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
                ent_cache = ext_dirent_populate(cache, ent->ino, ent->name, true);
            else
            {
                ent_cache = ZeroAllocate(EXT_Allocator, 1, sizeof(ext_dirent_cache) + ent->name_len, nullptr);
                ent_cache->ent = *ent;
                ent_cache->inode = ext_read_inode_pg(cache, ent->ino, &ent_cache->pg);
                memcpy(ent_cache->ent.name, ent->name, ent->name_len);
            }

            ent_cache->ent.file_type = ent->file_type;
            ent_cache->ent.name_len = ent->name_len;
            ent_cache->ent.rec_len = ent->rec_len;

            ext_dirent_adopt(parent, ent_cache);
        }

        down:
        offset += ent->rec_len;
    }

    MmH_DerefPage(pg);
    return parent;
}
