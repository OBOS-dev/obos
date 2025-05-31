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
#include <mm/swap.h>

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
    for (; i < OBOS_MIN(ext_ino_max_block_index(cache, inode), (size_t)12); i++)
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

uint32_t ext_ino_allocate(ext_cache* cache, const uint32_t* block_group_ptr)
{
    if (!cache)
        return 0;
    uint32_t block_group = block_group_ptr ? *block_group_ptr : 0;
    bool checked_bg_zero = block_group == 0;
    if (block_group > cache->block_group_count)
        return 0;
    uint32_t res = 0;
    while (!res)
    {
        if (!cache->bgdt[block_group].free_inodes)
            goto down;

        uint32_t local_inode_index = 0;
        size_t bitmap_size = cache->inodes_per_block / 8;
        uint8_t* buffer = nullptr;
        uint32_t block = 0;
        page* pg = nullptr;
        bool found = false;
        for (size_t i = 0; i < bitmap_size; i++)
        {
            if ((i % cache->block_size) == 0)
            {
                if (pg)
                    MmH_DerefPage(pg);
                block = cache->bgdt[block_group].inode_bitmap + (i / cache->block_size);
                buffer = ext_read_block(cache, block, &pg);
                MmH_RefPage(pg);
            }
            if (buffer[i] != 0xff)
            {
                uint8_t bit = __builtin_ctz(~buffer[i]);
                local_inode_index = bit + i*8;
                found = true;
                buffer[i] |= BIT(bit);
                Mm_MarkAsDirtyPhys(pg);
                MmH_DerefPage(pg);
                break;
            }
        }
        if (found)
        {
            res = local_inode_index + cache->inodes_per_group*block_group + 1;
            continue;
        }

        down:
        if (++block_group >= cache->block_group_count)
        {
            if (checked_bg_zero)
                break;
            else
            {
                block_group = 0;
                checked_bg_zero = true;
            }
        }
    }

    return res;
}

void ext_ino_free(ext_cache* cache, uint32_t ino)
{
    if (!cache || !ino)
        return;

    ext_bgd* bgd = &cache->bgdt[ext_ino_get_block_group(cache, ino)];
    uint32_t local_inode_index = ext_ino_get_local_index(cache, ino);
    uint32_t inode_bitmap_block = le32_to_host(bgd->inode_bitmap) + local_inode_index / cache->inodes_per_block;
    uint32_t real_inode_index = (local_inode_index % cache->inodes_per_block);

    bool free = false;
    page* pg = nullptr;
    uint8_t* inode_bitmap = ext_read_block(cache, inode_bitmap_block, &pg);
    if (~inode_bitmap[real_inode_index / 8] & BIT(real_inode_index % 8))
        free = true;
    else
    {
        inode_bitmap[real_inode_index / 8] &= ~BIT(real_inode_index % 8);
        Mm_MarkAsDirtyPhys(pg);
    }
    MmH_DerefPage(pg);
    if (free)
        return;

    pg = nullptr;
    ext_inode* inode = ext_read_inode_pg(cache, ino, &pg);
    MmH_RefPage(pg);
    memzero(inode, sizeof(*inode));
    Mm_MarkAsDirtyPhys(pg);
    MmH_DerefPage(pg);
}