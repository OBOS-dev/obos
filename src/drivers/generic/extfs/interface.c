/*
 * drivers/generic/extfs/interface.c
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * Abandon all hope ye who enter here
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <mm/page.h>
#include <mm/swap.h>

#include <allocators/base.h>

#include <locks/spinlock.h>

#include "structs.h"

#define get_handle(desc) ({\
    if (!desc) return OBOS_STATUS_INVALID_ARGUMENT;\
    (ext_inode_handle*)desc;\
})

obos_status set_file_perms(dev_desc desc, driver_file_perm newperm)
{
    ext_inode_handle* hnd = get_handle(desc);
    // printf("%s: acquiring inode %d lock\n", __func__, hnd->ino);
    irql oldIrql = Core_SpinlockAcquire(&hnd->lock);
    page* pg = nullptr;
    ext_inode* ino = ext_read_inode_pg(hnd->cache, hnd->ino, &pg);
    MmH_RefPage(pg);
    
    uint32_t new_mode = ino->mode & ~0777;

    if (newperm.other_exec)
        new_mode |= EXT_OTHER_EXEC;
    if (newperm.owner_exec)
        new_mode |= EXT_OWNER_EXEC;
    if (newperm.group_exec)
        new_mode |= EXT_GROUP_EXEC;

    if (newperm.other_write)
        new_mode |= EXT_OTHER_WRITE;
    if (newperm.owner_write)
        new_mode |= EXT_OWNER_WRITE;
    if (newperm.group_write)
        new_mode |= EXT_GROUP_WRITE;
    
    if (newperm.other_read)
        new_mode |= EXT_OTHER_READ;
    if (newperm.owner_read)
        new_mode |= EXT_OWNER_READ;
    if (newperm.group_read)
        new_mode |= EXT_GROUP_READ;

    ino->mode = new_mode;
    Mm_MarkAsDirtyPhys(pg);
    MmH_DerefPage(pg);
    // printf("%s: releasing inode %d lock\n", __func__, hnd->ino);
    Core_SpinlockRelease(&hnd->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}

OBOS_WEAK obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    ext_inode_handle* hnd = (void*)desc;
    if (!hnd || !count)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ext_inode* node = ext_read_inode(hnd->cache, hnd->ino);
    if (!node)
        return OBOS_STATUS_INVALID_ARGUMENT; // uh oh :D
    *count = node->size;
    Free(EXT_Allocator, node, sizeof(*node));
    return OBOS_STATUS_SUCCESS;    
}

obos_status stat_fs_info(void *vn, drv_fs_info *info)
{
    if (!info)
        return OBOS_STATUS_INVALID_ARGUMENT;

    ext_cache *cache = nullptr;
    for (ext_cache* curr = LIST_GET_HEAD(ext_cache_list, &EXT_CacheList); curr && !cache; )
    {
        if (curr->vn == vn)
            cache = curr;

        curr = LIST_GET_NEXT(ext_cache_list, &EXT_CacheList, curr);
    }

    if (!cache)
        return OBOS_STATUS_NOT_FOUND;

    info->fsBlockSize = cache->block_size;
    info->freeBlocks = le32_to_host(cache->superblock.block_count) - le32_to_host(cache->superblock.free_block_count);

    info->availableFiles = le32_to_host(cache->superblock.free_inode_count);
    info->fileCount = le32_to_host(cache->superblock.inode_count) - le32_to_host(cache->superblock.free_inode_count);

    info->nameMax = 255;

    if (cache->read_only)
        info->flags |= FS_FLAGS_RDONLY;

    info->partBlockSize = cache->vn->blkSize;
    info->szFs = cache->vn->filesize / info->partBlockSize;

    return OBOS_STATUS_SUCCESS;
}