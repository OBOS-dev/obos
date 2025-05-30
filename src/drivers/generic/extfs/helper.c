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

static void ext_ino_foreach_indirect_block(ext_cache* cache,
                                           ext_inode* inode,
                                           iterate_decision(*cb)(ext_cache* cache, ext_inode* inode, uint32_t block, void* userdata),
                                           void* userdata,
                                           uint32_t *blocks, size_t *curr_index)
{
    size_t nEntriesPerBlock = cache->block_size / 4;
    size_t i = 0;
    for (; i < OBOS_MIN(ext_ino_max_block_index(cache, inode) - 12, nEntriesPerBlock); i++)
    {
        if (cb(cache, inode, blocks[i], userdata) == ITERATE_DECISION_STOP)
            break;
        (*curr_index)++;
        if ((*curr_index) >= ext_ino_max_block_index(cache, inode))
            break;
    }
}

static void ext_ino_foreach_doubly_indirect_block(ext_cache* cache,
                                                  ext_inode* inode,
                                                  iterate_decision(*cb)(ext_cache* cache, ext_inode* inode, uint32_t block, void* userdata),
                                                  void* userdata,
                                                  uint32_t *blocks, size_t *curr_index)
{
    size_t nEntriesPerBlock = cache->block_size / 4, i = 0;
    size_t maxBlockIndexDouble = ext_ino_max_block_index(cache, inode) / nEntriesPerBlock;
    if (ext_ino_max_block_index(cache, inode) % nEntriesPerBlock)
        maxBlockIndexDouble++;
    for (; i < OBOS_MIN(maxBlockIndexDouble, nEntriesPerBlock); i++)
    {
        page* pg = nullptr;
        uint32_t* indirect_blocks = ext_read_block(cache, le32_to_host(blocks[i]), &pg);
        MmH_RefPage(pg);
        ext_ino_foreach_indirect_block(cache,inode,cb,userdata,indirect_blocks,curr_index);
        MmH_DerefPage(pg);
        if (*curr_index >= ext_ino_max_block_index(cache, inode))
            return;
    }
}

static void ext_ino_foreach_triply_indirect_block(ext_cache* cache,
                                                  ext_inode* inode,
                                                  iterate_decision(*cb)(ext_cache* cache, ext_inode* inode, uint32_t block, void* userdata),
                                                  void* userdata,
                                                  uint32_t *blocks, size_t *curr_index)
{
    size_t nEntriesPerBlock = cache->block_size / 4, i = 0;
    size_t maxBlockIndexTriple = ext_ino_max_block_index(cache, inode) / nEntriesPerBlock / nEntriesPerBlock;
    if ((ext_ino_max_block_index(cache, inode) / nEntriesPerBlock) % nEntriesPerBlock)
        maxBlockIndexTriple++;
    for (; i < OBOS_MIN(maxBlockIndexTriple, nEntriesPerBlock); i++)
    {
        page* pg = nullptr;
        uint32_t* doubly_indirect_blocks = ext_read_block(cache, le32_to_host(blocks[i]), &pg);
        MmH_RefPage(pg);
        ext_ino_foreach_doubly_indirect_block(cache,inode,cb,userdata,doubly_indirect_blocks,curr_index);
        MmH_DerefPage(pg);
        if (*curr_index >= ext_ino_max_block_index(cache, inode))
            return;
    }
}

void ext_ino_foreach_block(ext_cache* cache,
                           ext_inode* inode,
                           iterate_decision(*cb)(ext_cache* cache, ext_inode* inode, uint32_t block, void* userdata), 
                           void* userdata)
{
    if (!cache || !inode || !cb)
        return;
    size_t i = 0;
    for (; i < OBOS_MIN(ext_ino_max_block_index(cache, inode), 12); i++)
        if (cb(cache, inode, inode->direct_blocks[i], userdata) == ITERATE_DECISION_STOP)
            break;

    if (i >= ext_ino_max_block_index(cache, inode))
        return; // We done here

    page* pg = nullptr;
    uint32_t* indirect_blocks = ext_read_block(cache, le32_to_host(inode->indirect_block), &pg);
    MmH_RefPage(pg);
    ext_ino_foreach_indirect_block(cache,inode,cb,userdata,indirect_blocks,&i);
    MmH_DerefPage(pg);

    if (i >= ext_ino_max_block_index(cache, inode))
        return; // We done here

    pg = nullptr;
    uint32_t* doubly_indirect_blocks = ext_read_block(cache, le32_to_host(inode->doubly_indirect_block), &pg);
    MmH_RefPage(pg);
    ext_ino_foreach_doubly_indirect_block(cache,inode,cb,userdata,doubly_indirect_blocks,&i);
    MmH_DerefPage(pg);

    if (i >= ext_ino_max_block_index(cache, inode))
        return; // We done here

    pg = nullptr;
    uint32_t* triply_indirect_blocks = ext_read_block(cache, le32_to_host(inode->triply_indirect_block), &pg);
    MmH_RefPage(pg);
    ext_ino_foreach_triply_indirect_block(cache,inode,cb,userdata,triply_indirect_blocks,&i);
    MmH_DerefPage(pg);
}
