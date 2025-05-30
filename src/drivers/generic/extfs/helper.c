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

#include <driver_interface/header.h>

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
    uint32_t inode_bitmap_block = le32_to_host(bgd->inode_bitmap) + local_inode_index / cache->inodes_per_block;
    uint32_t real_inode_index = (local_inode_index % cache->inodes_per_block);

    bool free = false;
    page* pg2 = nullptr;
    uint8_t* inode_bitmap = ext_read_block(cache, inode_bitmap_block, &pg2);
    if (~inode_bitmap[real_inode_index / 8] & BIT(real_inode_index % 8))
        free = true;
    MmH_DerefPage(pg2);
    if (free)
        return nullptr;

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

static iterate_decision ext_ino_foreach_indirect_block(ext_cache* cache,
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
            return ITERATE_DECISION_STOP;
        (*curr_index)++;
        if ((*curr_index) >= ext_ino_max_block_index(cache, inode))
            break;
    }
    return ITERATE_DECISION_CONTINUE;
}

static iterate_decision ext_ino_foreach_doubly_indirect_block(ext_cache* cache,
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
        if (ext_ino_foreach_indirect_block(cache,inode,cb,userdata,indirect_blocks,curr_index) == ITERATE_DECISION_STOP)
        {
            MmH_DerefPage(pg);
            return ITERATE_DECISION_STOP;
        }
        MmH_DerefPage(pg);
        if (*curr_index >= ext_ino_max_block_index(cache, inode))
            return ITERATE_DECISION_STOP;
    }
    return ITERATE_DECISION_CONTINUE;
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
        if (ext_ino_foreach_doubly_indirect_block(cache,inode,cb,userdata,doubly_indirect_blocks,curr_index) == ITERATE_DECISION_STOP)
        {
            MmH_DerefPage(pg);
            return;
        }
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

struct read_block_packet {
    void* buffer;
    size_t buffer_offset;
    size_t count;
    size_t start_offset;
    size_t current_offset;
    obos_status status;
};

static iterate_decision read_cb(ext_cache* cache, ext_inode* inode, uint32_t block, void* userdata)
{
    OBOS_UNUSED(inode);
    struct read_block_packet *packet = userdata;
    iterate_decision decision = ITERATE_DECISION_CONTINUE;
    if (packet->current_offset < packet->start_offset)
        goto out1;
    if (packet->current_offset > (packet->start_offset + packet->count))
    {
        decision = ITERATE_DECISION_STOP;
        goto out1;
    }
    void* const buff = (void*)((uintptr_t)packet->buffer + packet->buffer_offset);
    if (!block)
        memzero(buff, cache->block_size); // sparse block
    else
    {
        page* pg = nullptr;
        void* data = ext_read_block(cache, block, &pg);
        MmH_RefPage(pg);
        memcpy(buff, data, OBOS_MIN(packet->count - packet->buffer_offset, cache->block_size));
        MmH_DerefPage(pg);
    }
    packet->buffer_offset += cache->block_size;

    out1:
    packet->current_offset += cache->block_size;
    return decision;
}

obos_status ext_ino_read_blocks(ext_cache* cache, ext_inode* inode, size_t offset, size_t count, void* buffer, size_t *nRead)
{
    if (!cache || !inode || !buffer)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if ((offset + count) > inode->blocks*512)
        count = (offset + count) - inode->blocks*512;
    struct read_block_packet userdata = {.buffer=buffer,.start_offset=offset,.count=count,.buffer_offset=0};
    ext_ino_foreach_block(cache, inode, read_cb, &userdata);
    if (nRead)
        *nRead = userdata.buffer_offset;
    return userdata.status;
}
