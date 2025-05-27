/*
 * drivers/generic/extfs/helper.c
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * Abandon all hope, ye who enter here.
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <vfs/vnode.h>
#include <vfs/irp.h>

#include <allocators/base.h>

#include <mm/page.h>

#include "structs.h"

ext_inode* ext_read_inode_pg(ext_cache* cache, uint32_t ino, page** pg)
{
    if (!cache || !ino || !pg)
        return nullptr;
    if (ext_ino_get_block_group(cache, ino) > cache->block_group_count)
        return nullptr;
    ext_bgd* bgd = &cache->bgdt[ext_ino_get_block_group(cache, ino)];
    uint32_t local_inode_index = ext_ino_get_local_index(cache, ino);
    uint32_t inode_table_block = le32_to_host(bgd->inode_table) + local_inode_index / cache->inodes_per_block;
    uint32_t real_inode_index = (local_inode_index % cache->inodes_per_block);
    ext_inode* inodes = ext_read_block(cache, inode_table_block, pg);
    return (ext_inode*)((uintptr_t)inodes + real_inode_index * cache->inode_size);
}

ext_inode* ext_read_inode(ext_cache* cache, uint32_t ino)
{
    page* pg = nullptr;
    ext_inode* read = ext_read_inode_pg(cache, ino, &pg);
    if (!read)
        return read;
    ext_inode* ret = ZeroAllocate(EXT_Allocator, 1, sizeof(ext_inode), nullptr);
    memcpy(ret, read, sizeof(ext_inode));
    MmH_DerefPage(pg);
    return ret;
}