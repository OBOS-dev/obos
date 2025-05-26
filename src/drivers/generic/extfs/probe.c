/*
 * drivers/generic/extfs/probe.c
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * Abandon all hope ye who enter here
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

bool probe(void* vn_)
{
    OBOS_ASSERT(vn_);
    if (!vn_)
        return false;
    vnode* vn = vn_;
    ext_cache* cache = ZeroAllocate(EXT_Allocator, 1, sizeof(ext_cache), nullptr);
    cache->block_size = 1024;
    cache->vn = vn;
    page* pg = nullptr;
    ext_superblock* sb = ext_read_block(cache, 1, &pg);
    MmH_RefPage(pg);
    if (!sb)
    {
        MmH_DerefPage(pg);
        Free(EXT_Allocator, cache, sizeof(*cache));
        return false;
    }
    if (sb->magic != EXT_MAGIC)
    {
        MmH_DerefPage(pg);
        Free(EXT_Allocator, cache, sizeof(*cache));
        return false;
    }
    memcpy(&cache->superblock, sb, sizeof(*sb));
    MmH_DerefPage(pg);
    sb = &cache->superblock;
    pg = nullptr;
    cache->block_size = ext_sb_block_size(sb);   
    cache->blocks_per_group = ext_sb_blocks_per_group(sb);
    cache->inodes_per_group = ext_sb_inodes_per_group(sb);
    cache->inode_size = ext_sb_inode_size(sb);
    cache->revision = le32_to_host(sb->revision);
    cache->block_group_count = le32_to_host(cache->superblock.block_count) / cache->blocks_per_group;
    cache->vn = vn;
    cache->bgdt = Allocate(EXT_Allocator, cache->block_group_count * sizeof(ext_bgd), nullptr);

    // Populate the in-memory BGDT
    do {
        uint32_t bgdt_blocks = (cache->block_group_count * sizeof(ext_bgd)) / cache->block_size;
        if ((cache->block_group_count * sizeof(ext_bgd)) % cache->block_size)
            bgdt_blocks++;
        for (size_t i = 0; i < bgdt_blocks; i++)
        {
            void* bgdt_section = ext_read_block(cache, (cache->block_size == 1024 ? 2 : 1) + i, &pg);
            MmH_RefPage(pg);
            size_t copy_count = i == (bgdt_blocks - 1) ? (cache->block_group_count % (cache->block_size / sizeof(ext_bgd))) : (cache->block_size / sizeof(ext_bgd));
            memcpy(cache->bgdt, bgdt_section, copy_count*sizeof(ext_bgd));
            MmH_DerefPage(pg);
        }
    } while(0);

    OBOS_Debug("extfs: Block size: 0x%x\n", cache->block_size);
    OBOS_Debug("extfs: Blocks per group: 0x%x\n", cache->blocks_per_group);
    OBOS_Debug("extfs: Inodes per group: 0x%x\n", cache->inodes_per_group);
    OBOS_Debug("extfs: Inode size: 0x%x\n", cache->inode_size);
    OBOS_Debug("extfs: Block group count: 0x%d\n", cache->block_group_count);
    OBOS_Debug("extfs: Revision: %d\n", cache->revision);
    return true;
}