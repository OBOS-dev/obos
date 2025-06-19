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
    uint32_t real_inode_index = (local_inode_index % cache->inodes_per_block);
    uint32_t inode_bitmap_block = le32_to_host(bgd->inode_bitmap) + (local_inode_index / 8) / (cache->inodes_per_group/8);
    uint32_t bmp_idx = (local_inode_index / 8) % (cache->inodes_per_group/8);

    bool free = false;
    page* pg2 = nullptr;
    uint8_t* inode_bitmap = ext_read_block(cache, inode_bitmap_block, &pg2);
    if (~inode_bitmap[bmp_idx] & BIT(local_inode_index % 8))
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
                                           iterate_decision(*cb)(ext_cache* cache, ext_inode* inode, uint32_t *block, void* userdata),
                                           void* userdata,
                                           uint32_t *blocks, size_t *curr_index, bool *dirty)
{
    size_t nEntriesPerBlock = cache->block_size / 4;
    size_t i = 0;
    for (; i < OBOS_MIN(ext_ino_max_block_index(cache, inode) - 12, nEntriesPerBlock); i++)
    {
        uint32_t blk = blocks ? blocks[i] : 0;
        if (cb(cache, inode, blocks ? &blocks[i] : 0, userdata) == ITERATE_DECISION_STOP)
        {
            if (!(*dirty))
                *dirty = blocks ? blocks[i] != blk : 0;
            break;
        }
        if (!(*dirty))
            *dirty = blocks ? blocks[i] != blk : 0;
        (*curr_index)++;
        if ((*curr_index) >= ext_ino_max_block_index(cache, inode))
            break;
    }
    return ITERATE_DECISION_CONTINUE;
}

static iterate_decision ext_ino_foreach_doubly_indirect_block(ext_cache* cache,
                                                  ext_inode* inode,
                                                  iterate_decision(*cb)(ext_cache* cache, ext_inode* inode, uint32_t *block, void* userdata),
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
        uint32_t* indirect_blocks = nullptr;
        if (blocks)
            indirect_blocks = blocks[i] ? ext_read_block(cache, le32_to_host(blocks[i]), &pg) : nullptr;
        else
            indirect_blocks = nullptr;
        MmH_RefPage(pg);
        bool dirty = false;
        if (ext_ino_foreach_indirect_block(cache,inode,cb,userdata,indirect_blocks,curr_index, &dirty) == ITERATE_DECISION_STOP)
        {
            MmH_DerefPage(pg);
            return ITERATE_DECISION_STOP;
        }
        if (dirty)
            Mm_MarkAsDirtyPhys(pg);
        MmH_DerefPage(pg);
        if (*curr_index >= ext_ino_max_block_index(cache, inode))
            return ITERATE_DECISION_STOP;
    }
    return ITERATE_DECISION_CONTINUE;
}

static void ext_ino_foreach_triply_indirect_block(ext_cache* cache,
                                                  ext_inode* inode,
                                                  iterate_decision(*cb)(ext_cache* cache, ext_inode* inode, uint32_t *block, void* userdata),
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
        uint32_t* doubly_indirect_blocks =  blocks[i] ? ext_read_block(cache, le32_to_host(blocks[i]), &pg) : nullptr;
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
                           uint32_t ino,
                           iterate_decision(*cb)(ext_cache* cache, ext_inode* inode, uint32_t *block, void* userdata), 
                           void* userdata)
{
    if (!cache || !ino || !cb)
        return;

    page* pg = nullptr;

    ext_inode* inode = ext_read_inode_pg(cache, ino, &pg);
    MmH_RefPage(pg);

    size_t i = 0;
    for (; i < OBOS_MIN(ext_ino_max_block_index(cache, inode), (size_t)12); i++)
    {
        uint32_t blk = inode->direct_blocks[i];
        if (cb(cache, inode, &inode->direct_blocks[i], userdata) == ITERATE_DECISION_STOP)
        {
            if (blk != inode->direct_blocks[i])
                Mm_MarkAsDirtyPhys(pg);
            break;
        }
    }

    if (i >= ext_ino_max_block_index(cache, inode))
    {
        MmH_DerefPage(pg);
        return; // We done here
    }

    page* pg2 = nullptr;
    uint32_t* indirect_blocks = ext_read_block(cache, le32_to_host(inode->indirect_block), &pg2);
    MmH_RefPage(pg2);
    bool dirty = false;
    ext_ino_foreach_indirect_block(cache,inode,cb,userdata,indirect_blocks,&i,&dirty);
    if (dirty)
        Mm_MarkAsDirtyPhys(pg2);
    MmH_DerefPage(pg2);

    if (i >= ext_ino_max_block_index(cache, inode))
    {
        MmH_DerefPage(pg);
        return; // We done here
    }

    pg2 = nullptr;
    uint32_t* doubly_indirect_blocks = ext_read_block(cache, le32_to_host(inode->doubly_indirect_block), &pg2);
    MmH_RefPage(pg2);
    ext_ino_foreach_doubly_indirect_block(cache,inode,cb,userdata,doubly_indirect_blocks,&i);
    MmH_DerefPage(pg2);

    if (i >= ext_ino_max_block_index(cache, inode))
    {
        MmH_DerefPage(pg);
        return; // We done here
    }

    pg2 = nullptr;
    uint32_t* triply_indirect_blocks = ext_read_block(cache, le32_to_host(inode->triply_indirect_block), &pg2);
    MmH_RefPage(pg2);
    ext_ino_foreach_triply_indirect_block(cache,inode,cb,userdata,triply_indirect_blocks,&i);
    MmH_DerefPage(pg2);
}

struct read_block_packet {
    void* buffer;
    size_t buffer_offset;
    size_t count;
    size_t start_offset;
    size_t current_offset;
    obos_status status;
};

static iterate_decision read_cb(ext_cache* cache, ext_inode* inode, uint32_t *block, void* userdata)
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
        void* data = ext_read_block(cache, *block, &pg);
        MmH_RefPage(pg);
        memcpy(buff, data, OBOS_MIN(packet->count - packet->buffer_offset, cache->block_size));
        MmH_DerefPage(pg);
    }
    packet->buffer_offset += cache->block_size;

    out1:
    packet->current_offset += cache->block_size;
    return decision;
}

obos_status ext_ino_read_blocks(ext_cache* cache, uint32_t ino, size_t offset, size_t count, void* buffer, size_t *nRead)
{
    if (!cache || !ino || !buffer)
        return OBOS_STATUS_INVALID_ARGUMENT;
    page* pg = nullptr;
    ext_inode* inode = ext_read_inode_pg(cache, ino, &pg);
    MmH_RefPage(pg);
    if (!inode)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if ((offset + count) > inode->blocks*512)
        count = (offset + count) - inode->blocks*512;
    struct read_block_packet userdata = {.buffer=buffer,.start_offset=offset,.count=count,.buffer_offset=0};
    ext_ino_foreach_block(cache, ino, read_cb, &userdata);
    MmH_DerefPage(pg);
    if (nRead)
        *nRead = userdata.buffer_offset;
    return userdata.status;
}

struct commit_blks_packet
{
    // in blocks
    uint32_t minimum_offset;
    uint32_t maximum_offset;
    uint32_t current_offset;
    struct {
        uint32_t* arr;
        size_t cnt;
    } offsets_to_commit;
};


struct write_blks_packet
{
    // in blocks
    uint32_t minimum_offset;
    uint32_t maximum_offset;
    uint32_t current_offset;
    uint32_t block_group;
    const void* buff;
    size_t buff_sz;
    size_t buff_offset;
    uint32_t initial_offset;
};

iterate_decision write_blks_cb(ext_cache* cache, ext_inode* inode, uint32_t* block, void* userdata)
{
    OBOS_UNUSED(cache && inode);
    struct write_blks_packet* packet = userdata;
    uint32_t current_offset = packet->current_offset++;
    if (current_offset < packet->minimum_offset)
        return ITERATE_DECISION_CONTINUE;
    if (current_offset >= packet->maximum_offset)
        return ITERATE_DECISION_CONTINUE;
    if (!block)
        return ITERATE_DECISION_CONTINUE;
    
    page* pg = nullptr;
    char* data = ext_read_block(cache, *block, &pg);
    MmH_RefPage(pg);
    size_t copy_count = OBOS_MIN(packet->buff_sz-packet->buff_offset, cache->block_size); 
    memcpy(data+(current_offset == packet->minimum_offset ? packet->initial_offset : 0), 
    (char*)packet->buff+packet->buff_offset, 
    copy_count);
    packet->buff_offset += copy_count;
    Mm_MarkAsDirtyPhys(pg);
    MmH_DerefPage(pg);
    
    return ITERATE_DECISION_CONTINUE;
}

obos_status ext_ino_write_blocks(ext_cache* cache, uint32_t ino, size_t offset, size_t count, const void* buffer, size_t *nWritten)
{
    if (!cache || !ino || !buffer)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (cache->read_only)
        return OBOS_STATUS_READ_ONLY;
    
    page* pg = nullptr;
    ext_inode* inode = ext_read_inode_pg(cache, ino, &pg);
    MmH_RefPage(pg);
    size_t inode_size = inode->size;
#if ext_sb_supports_64bit_filesize
    inode_size |= ((uint64_t)inode->dir_acl << 32);
#endif
    MmH_DerefPage(pg);
    if ((offset+count) > inode_size)
        return OBOS_STATUS_NO_SPACE;

    struct write_blks_packet packet = {
        .block_group = ext_ino_get_block_group(cache, ino),
        .minimum_offset=offset/cache->block_size,
        .maximum_offset=offset/cache->block_size+(count/cache->block_size + (count%cache->block_size ? 1 : 0)),
        .buff = buffer,
        .buff_sz = count,
        .initial_offset = offset % cache->block_size
    };
    ext_ino_foreach_block(cache, ino, write_blks_cb, &packet);

    if (nWritten)
        *nWritten = packet.buff_offset;

    return OBOS_STATUS_SUCCESS;
}

iterate_decision commit_blks_cb(ext_cache* cache, ext_inode* inode, uint32_t* block, void* userdata)
{
    OBOS_UNUSED(cache && inode);
    struct commit_blks_packet* packet = userdata;
    uint32_t current_offset = packet->current_offset++;
    if (current_offset < packet->minimum_offset)
        return ITERATE_DECISION_CONTINUE;
    if (current_offset >= packet->maximum_offset)
        return ITERATE_DECISION_CONTINUE;
    if (block)
        return ITERATE_DECISION_CONTINUE;
    size_t old_size = packet->offsets_to_commit.cnt++ * sizeof(uint32_t);
    size_t new_size = packet->offsets_to_commit.cnt * sizeof(uint32_t);
    packet->offsets_to_commit.arr = Reallocate(EXT_Allocator, packet->offsets_to_commit.arr, new_size, old_size, nullptr);
    packet->offsets_to_commit.arr[packet->offsets_to_commit.cnt] = current_offset;
    return ITERATE_DECISION_CONTINUE;
}

obos_status ext_ino_commit_blocks(ext_cache* cache, uint32_t ino, size_t offset, size_t size)
{
    if (!cache || !ino)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!size)
        return OBOS_STATUS_SUCCESS;
    if (cache->read_only)
        return OBOS_STATUS_READ_ONLY;
    
    page* pg = nullptr;
    ext_inode* inode = ext_read_inode_pg(cache, ino, &pg);
    MmH_RefPage(pg);
    bool inode_dirty = false;
    
    uint32_t block_group = ext_ino_get_block_group(cache, ino);
    struct commit_blks_packet packet = {
        .minimum_offset=offset/cache->block_size,
        .maximum_offset=offset/cache->block_size+(size/cache->block_size + (size%cache->block_size ? 1 : 0))
    };
    ext_ino_foreach_block(cache, ino, commit_blks_cb, &packet);
    for (size_t i = 0; i < packet.offsets_to_commit.cnt; i++)
    {
        struct inode_offset_location loc = ext_get_blk_index_from_offset(cache, packet.offsets_to_commit.arr[i]);
        uint32_t block = ext_blk_allocate(cache, &block_group);
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
                : idx_doubly_indirect_block) // loc.idx[2] == UINT32_MAX 
            : idx_triply_indirect_block); // loc.idx[3] == UINT32_MAX
        switch (what) {
            case idx_blocks:
                inode->direct_blocks[loc.idx[0]] = block;
                inode_dirty = true;
                break;
            case idx_indirect_block:
            {
                if (!inode->indirect_block)
                {
                    inode->indirect_block = ext_blk_allocate(cache, &block_group);
                    inode_dirty = true;
                }
                page* pg2 = nullptr;
                uint32_t* indirect_block = ext_read_block(cache, inode->indirect_block, &pg2);
                MmH_RefPage(pg2);
                indirect_block[loc.idx[1]] = block;
                MmH_DerefPage(pg2);
                break;
            }
            case idx_doubly_indirect_block:
            {
                if (!inode->doubly_indirect_block)
                {
                    inode->doubly_indirect_block = ext_blk_allocate(cache, &block_group);
                    inode_dirty = true;
                }
                page* pg2 = nullptr;
                uint32_t* doubly_indirect_block = ext_read_block(cache, inode->doubly_indirect_block, &pg2);
                MmH_RefPage(pg2);
                if (!doubly_indirect_block[loc.idx[2]])
                {
                    doubly_indirect_block[loc.idx[2]] = ext_blk_allocate(cache, &block_group);
                    Mm_MarkAsDirtyPhys(pg2);
                }
                uint32_t indirect_block_number = doubly_indirect_block[loc.idx[2]];
                MmH_DerefPage(pg2);
                uint32_t* indirect_block = ext_read_block(cache, indirect_block_number, &pg2);
                MmH_RefPage(pg2);
                indirect_block[loc.idx[1]] = block;
                MmH_DerefPage(pg2);
                break;
            }
            case idx_triply_indirect_block:
            {
                if (!inode->triply_indirect_block)
                {
                    inode->triply_indirect_block = ext_blk_allocate(cache, &block_group);
                    inode_dirty = true;
                }
                page* pg2 = nullptr;
                uint32_t* triply_indirect_block = ext_read_block(cache, inode->triply_indirect_block, &pg2);
                MmH_RefPage(pg2);
                if (!triply_indirect_block[loc.idx[3]])
                {
                    triply_indirect_block[loc.idx[3]] = ext_blk_allocate(cache, &block_group);
                    Mm_MarkAsDirtyPhys(pg2);
                }
                uint32_t doubly_indirect_block_number = triply_indirect_block[loc.idx[2]];
                MmH_DerefPage(pg2);
                uint32_t* doubly_indirect_block = ext_read_block(cache, doubly_indirect_block_number, &pg2);
                MmH_RefPage(pg2);
                if (!doubly_indirect_block[loc.idx[2]])
                {
                    doubly_indirect_block[loc.idx[2]] = ext_blk_allocate(cache, &block_group);
                    Mm_MarkAsDirtyPhys(pg2);
                }
                uint32_t indirect_block_number = doubly_indirect_block[loc.idx[2]];
                MmH_DerefPage(pg2);
                uint32_t* indirect_block = ext_read_block(cache, indirect_block_number, &pg2);
                MmH_RefPage(pg2);
                indirect_block[loc.idx[1]] = block;
                MmH_DerefPage(pg2);
                break;
            }
        }
    }

    if (inode_dirty)
        Mm_MarkAsDirtyPhys(pg);
    MmH_DerefPage(pg);
    
    return OBOS_STATUS_SUCCESS;
}

#undef idx_blocks
#undef idx_indirect_block
#undef idx_doubly_indirect_block
#undef idx_triply_indirect_block

obos_status ext_ino_resize(ext_cache* cache, uint32_t ino, size_t new_size, bool expand_only)
{
    if (!cache || !ino)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (cache->read_only)
        return OBOS_STATUS_READ_ONLY;
    
    page* pg = nullptr;
    ext_inode* inode = ext_read_inode_pg(cache, ino, &pg);
    MmH_RefPage(pg);
    uint32_t blocks = new_size/512;
    if (new_size % 512)
        blocks++;
    if (blocks % (cache->block_size / 512))
        blocks += (cache->block_size / 512) - (blocks % (cache->block_size / 512));
    size_t inode_size = inode->size;
#if ext_sb_supports_64bit_filesize
    inode_size |= ((uint64_t)inode->dir_acl << 32);
#endif
    if (blocks == inode->blocks && new_size == inode_size) 
    {
        MmH_DerefPage(pg);
        return OBOS_STATUS_SUCCESS;
    }
    if (expand_only && new_size < inode_size)
    {
        MmH_DerefPage(pg);
        return OBOS_STATUS_SUCCESS;
    }
    
    inode->blocks = blocks;
    inode->size = new_size & 0xffffffff;
#if ext_sb_supports_64bit_filesize
    inode->dir_acl = new_size >> 32;
#endif

    Mm_MarkAsDirtyPhys(pg);
    MmH_DerefPage(pg);
    
    return OBOS_STATUS_SUCCESS;
}

char* ext_ino_get_linked(ext_cache* cache, ext_inode* inode, uint32_t ino_num)
{
    if (!cache || !inode || !ino_num)
        return nullptr;
    int ea_blocks = le32_to_host(inode->file_acl) ? (cache->block_size>>9) : 0;
    bool fast_symlink = !(le32_to_host(inode->blocks) - ea_blocks);
    char* ret = nullptr;
    if (fast_symlink)
    {
        size_t str_len = strnlen((char*)&inode->direct_blocks, 60);
        ret = Allocate(EXT_Allocator, str_len+1, nullptr);
        memcpy(ret, (char*)&inode->direct_blocks, str_len);
        ret[str_len] = 0;
    }
    else
    {
        // Read blocks
        void* buff = Allocate(EXT_Allocator, le32_to_host(inode->blocks) * 512, nullptr);
        ext_ino_read_blocks(cache, ino_num, 0, le32_to_host(inode->blocks) * 512, buff, nullptr);
        size_t str_len = strnlen((char*)&inode->direct_blocks, le32_to_host(inode->blocks) * 512);
        ret = Reallocate(EXT_Allocator, buff, str_len+1, le32_to_host(inode->blocks) * 512, nullptr);
        ret[str_len] = 0;
    }
    return ret;
}

struct inode_offset_location ext_get_blk_index_from_offset(ext_cache* cache, size_t offset)
{
    struct inode_offset_location loc = {.offset=offset,.idx={}};
    memset(loc.idx, 0xff, sizeof(loc.idx));
    if (offset < (cache->block_size*12))
    {
        loc.idx[0] = offset / cache->block_size;
        return loc;
    }
    offset -= (cache->block_size*12);
    const size_t nBlocksPerIndirectBlock = cache->block_size / 4;
    loc.idx[1] = (offset % cache->block_size*nBlocksPerIndirectBlock) / cache->block_size;
    if (offset < (cache->block_size*nBlocksPerIndirectBlock))
        return loc;
    offset -= (cache->block_size*nBlocksPerIndirectBlock);
    const size_t nBlocksPerDoublyIndirectBlock = nBlocksPerIndirectBlock*nBlocksPerIndirectBlock;
    loc.idx[2] = (offset % cache->block_size*nBlocksPerDoublyIndirectBlock) / cache->block_size;
    if (offset < (cache->block_size*nBlocksPerDoublyIndirectBlock))
        return loc;
    offset -= (cache->block_size*nBlocksPerDoublyIndirectBlock);
    const size_t nBlocksPerTriplyIndirectBlock = nBlocksPerDoublyIndirectBlock*nBlocksPerIndirectBlock;
    loc.idx[3] = (offset % cache->block_size*nBlocksPerTriplyIndirectBlock) / cache->block_size;
    return loc;
}

enum {
    ALLOC_INODE = false,
    ALLOC_BLOCK = true,
};

uint32_t allocate_bmp(ext_cache* cache, const uint32_t* block_group_ptr, int what)
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
        if (!(what == ALLOC_INODE ? cache->bgdt[block_group].free_inodes : cache->bgdt[block_group].free_blocks))
            goto down;

        uint32_t local_index = 0;
        size_t bitmap_size = what == ALLOC_INODE ? cache->inodes_per_block / 8 : cache->blocks_per_group / 8;
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
                uint32_t bmp = what == ALLOC_INODE ? cache->bgdt[block_group].inode_bitmap : cache->bgdt[block_group].block_bitmap;
                block = bmp + (i / cache->block_size);
                buffer = ext_read_block(cache, block, &pg);
                MmH_RefPage(pg);
            }
            if (buffer[i] != 0xff)
            {
                uint8_t bit = __builtin_ctz(~buffer[i]);
                local_index = bit + i*8;
                found = true;
                buffer[i] |= BIT(bit);
                Mm_MarkAsDirtyPhys(pg);
                MmH_DerefPage(pg);
                break;
            }
        }
        if (found)
        {
            res = local_index + (what==ALLOC_INODE ? cache->inodes_per_group : cache->blocks_per_group) * block_group + 1;
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

uint32_t ext_ino_allocate(ext_cache* cache, const uint32_t* block_group_ptr)
{
    return allocate_bmp(cache, block_group_ptr, ALLOC_INODE);
}

void ext_ino_free(ext_cache* cache, uint32_t ino)
{
    if (!cache || !ino)
        return;

    ext_bgd* bgd = &cache->bgdt[ext_ino_get_block_group(cache, ino)];
    uint32_t local_inode_index = ext_ino_get_local_index(cache, ino);
    uint32_t inode_bitmap_block = le32_to_host(bgd->inode_bitmap) + (local_inode_index / 8) / (cache->inodes_per_group/8);
    uint32_t idx = (local_inode_index / 8) % (cache->inodes_per_group/8);
    
    bool free = false;
    page* pg = nullptr;
    uint8_t* inode_bitmap = ext_read_block(cache, inode_bitmap_block, &pg);
    MmH_RefPage(pg);
    if (~inode_bitmap[idx] & BIT(local_inode_index % 8))
        free = true;
    else
    {
        Mm_MarkAsDirtyPhys(pg);
        inode_bitmap[idx] &= ~BIT(local_inode_index % 8);
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

    bgd->free_inodes++;
    cache->superblock.free_inode_count++;
    ext_writeback_sb(cache);
    ext_writeback_bgd(cache, ext_ino_get_block_group(cache, ino));
}

uint32_t ext_blk_allocate(ext_cache* cache, const uint32_t* block_group_ptr)
{
    return allocate_bmp(cache, block_group_ptr, ALLOC_BLOCK);
}

void ext_blk_free(ext_cache* cache, uint32_t blk)
{
    ext_bgd* bgd = &cache->bgdt[blk/cache->blocks_per_group];
    uint32_t local_index = blk%cache->blocks_per_group;
    uint32_t bmp_block = le32_to_host(bgd->block_bitmap) + local_index / (cache->block_size * 8);
    uint32_t bit = (local_index % (cache->block_size * 8)) % 8;
    uint32_t idx = (local_index % (cache->block_size * 8)) / 8;
    
    page* pg = nullptr;
    uint8_t* bmp = ext_read_block(cache, bmp_block, &pg);
    MmH_RefPage(pg);
    if (bmp[idx] & BIT(bit))
    {
        bmp[idx] &= ~BIT(bit);
        Mm_MarkAsDirtyPhys(pg);
    }
    MmH_DerefPage(pg);

    bgd->free_blocks++;
    cache->superblock.free_block_count++;
    ext_writeback_sb(cache);
    ext_writeback_bgd(cache, blk/cache->blocks_per_group);
}

void ext_writeback_bgd(ext_cache* cache, uint32_t bgd_idx)
{
    page* pg = nullptr;
    ext_bgdt bgdt_section = ext_read_block(cache, (cache->block_size == 1024 ? 2 : 1) + (bgd_idx / (cache->block_size / sizeof(ext_bgd))), &pg);
    MmH_RefPage(pg);
    bgdt_section[bgd_idx % (cache->block_size / sizeof(ext_bgd))] = cache->bgdt[bgd_idx];
    Mm_MarkAsDirtyPhys(pg);
    MmH_DerefPage(pg);
}
void ext_writeback_sb(ext_cache* cache)
{
    page* pg = nullptr;
    void* sb = ext_read_block(cache, (cache->block_size == 1024 ? 1 : 0), &pg);
    MmH_RefPage(pg);
    memcpy(sb+(cache->block_size == 1024 ? 0 : 1024), &cache->superblock, sizeof(cache->superblock));
    Mm_MarkAsDirtyPhys(pg);
    MmH_DerefPage(pg);
}