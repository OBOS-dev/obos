/*
 * drivers/generic/fat/interface.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>
#include <vfs/limits.h>

#include <allocators/base.h>

#include <utils/list.h>
#include <utils/string.h>

#include <driver_interface/header.h>

#include <uacpi_libc.h>

#include <locks/mutex.h>

#include "structs.h"

OBOS_WEAK obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    if (!desc || !count)
        return OBOS_STATUS_INVALID_ARGUMENT;
    fat_dirent_cache* cache_entry = (fat_dirent_cache*)desc;
    *count = cache_entry->data.filesize;
    return OBOS_STATUS_SUCCESS;
}
obos_status read_sync(dev_desc desc, void* buf_, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    if (!desc || !buf_)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!blkCount)
        return OBOS_STATUS_SUCCESS;
    fat_dirent_cache* cache_entry = (fat_dirent_cache*)desc;
    fat_cache* cache = cache_entry->owner;
    if (blkOffset >= cache_entry->data.filesize)
    {
        *nBlkRead = 0;
        return OBOS_STATUS_SUCCESS;
    }
    size_t nToRead = blkCount;
    if ((blkOffset + blkCount) >= cache_entry->data.filesize)
        nToRead = cache_entry->data.filesize - blkOffset;
    size_t bytesPerCluster = (cache->bpb->sectorsPerCluster*cache->blkSize);
    size_t nClustersToRead = (nToRead / bytesPerCluster) + (((nToRead % bytesPerCluster) != 0) ? 1 : 0);
    uint32_t cluster = cache_entry->data.first_cluster_low;
    if (cache->fatType == FAT32_VOLUME)
        cluster |= ((uint32_t)cache_entry->data.first_cluster_high << 16);
    uint8_t* cluster_buf = OBOS_NonPagedPoolAllocator->Allocate(OBOS_NonPagedPoolAllocator, bytesPerCluster, nullptr);
    size_t current_offset = 0;
    size_t cluster_offset = blkOffset % bytesPerCluster;
    int64_t bytesLeft = blkCount;
    obos_status status = OBOS_STATUS_SUCCESS;
    uint8_t *buf = buf_;
    Core_MutexAcquire(&cache->fd_lock);
    for (size_t i = 0; i < nClustersToRead && bytesLeft > 0; i++)
    {
        Vfs_FdSeek(cache->volume, ClusterToSector(cache, cluster+i)*cache->blkSize, SEEK_SET);
        status = Vfs_FdRead(cache->volume, cluster_buf + cluster_offset, bytesPerCluster, nullptr);
        if (obos_is_error(status))
        {
            OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, cluster_buf, bytesPerCluster);
            Core_MutexRelease(&cache->fd_lock);
            return status;
        }
        memcpy(buf+current_offset, cluster_buf, bytesPerCluster > (size_t)bytesLeft ? bytesPerCluster : (size_t)bytesLeft);

        current_offset += bytesPerCluster;
        // cluster_offset += bytesPerCluster;
        cluster_offset = 0;
        bytesLeft -= bytesPerCluster;
    }
    Core_MutexRelease(&cache->fd_lock);
    return OBOS_STATUS_SUCCESS;
}
OBOS_WEAK obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);

obos_status query_path(dev_desc desc, const char** path)
{
    if (!desc || !path)
        return OBOS_STATUS_INVALID_ARGUMENT;
    fat_dirent_cache* cache_entry = (fat_dirent_cache*)desc;
    *path = OBOS_GetStringCPtr(&cache_entry->path);
    return OBOS_STATUS_SUCCESS;
}
obos_status path_search(dev_desc* found, void* vn_, const char* what)
{
    if (!found || !vn_ || !what)
        return OBOS_STATUS_INVALID_ARGUMENT;
    fat_cache* cache = nullptr;
    for (cache = LIST_GET_HEAD(fat_cache_list, &FATVolumes); cache; )
    {
        if (cache->vn == vn_)
            break;

        cache = LIST_GET_NEXT(fat_cache_list, &FATVolumes, cache);
    }
    if (!cache)
        return OBOS_STATUS_INVALID_OPERATION; // not a fat volume we have probed
    *found = (dev_desc)DirentLookupFrom(what, cache->root);
    if (*found)
        return OBOS_STATUS_SUCCESS;
    return OBOS_STATUS_NOT_FOUND;
}
obos_status get_linked_desc(dev_desc desc, dev_desc* found)
{
    OBOS_UNUSED(desc);
    OBOS_UNUSED(found);
    return OBOS_STATUS_INTERNAL_ERROR; // Impossible, as we don't know of such thing.
}
OBOS_WEAK obos_status move_desc_to(dev_desc desc, const char* where);
OBOS_WEAK obos_status mk_file(dev_desc* newDesc, dev_desc parent, const char* name, file_type type);
OBOS_WEAK obos_status remove_file(dev_desc desc);
OBOS_WEAK obos_status set_file_perms(dev_desc desc, driver_file_perm newperm);
obos_status get_file_perms(dev_desc desc, driver_file_perm *perm)
{
    if (!desc || !perm)
        return OBOS_STATUS_INVALID_ARGUMENT;
    fat_dirent_cache* cache_entry = (fat_dirent_cache*)desc;
    static const driver_file_perm base_perm = {
        .owner_read=true,
        .group_read=true,
        .other_read=false,
        .owner_write=true,
        .group_write=true,
        .other_write=false,
        .owner_exec=true,
        .group_exec=true,
        .other_exec=true,
    };
    *perm = base_perm;
    if (cache_entry->data.attribs & READ_ONLY)
    {
        perm->owner_write = false;
        perm->other_write = false;
        perm->group_write = false;
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status get_file_type(dev_desc desc, file_type *type)
{
    if (!desc || !type)
        return OBOS_STATUS_INVALID_ARGUMENT;
    fat_dirent_cache* cache_entry = (fat_dirent_cache*)desc;
    if (cache_entry->data.attribs & DIRECTORY)
        *type = FILE_TYPE_DIRECTORY;
    else
        *type = FILE_TYPE_REGULAR_FILE;
    return OBOS_STATUS_SUCCESS;
}
obos_status list_dir(dev_desc dir, void* vn, iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata), void* userdata)
{
    if (!dir || !vn || !cb)
        return OBOS_STATUS_INVALID_ARGUMENT;
    fat_cache* cache = nullptr;
    for (cache = LIST_GET_HEAD(fat_cache_list, &FATVolumes); cache; )
    {
        if (cache->vn == vn)
            break;

        cache = LIST_GET_NEXT(fat_cache_list, &FATVolumes, cache);
    }
    if (!cache)
        return OBOS_STATUS_INVALID_OPERATION; // not a fat volume we have probed
    if (dir == UINTPTR_MAX)
        dir = (dev_desc)cache->root;
    for (fat_dirent_cache* cache_entry = ((fat_dirent_cache*)dir)->fdc_children.head; cache_entry; )
    {
        if (cache_entry->data.attribs & VOLUME_ID)
        {
            cache_entry = cache_entry->fdc_next_child;
            continue;
        }
        OBOS_ASSERT(cache_entry->data.attribs != LFN);
        if (cb((dev_desc)cache_entry, 1, cache_entry->data.filesize, userdata) == ITERATE_DECISION_STOP)
            break;
        cache_entry = cache_entry->fdc_next_child;
    }
    return OBOS_STATUS_SUCCESS;
}
