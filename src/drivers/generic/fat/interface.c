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

#include "structs.h"

OBOS_WEAK obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    if (!desc || !count)
        return OBOS_STATUS_INVALID_ARGUMENT;
    fat_dirent_cache* cache_entry = (fat_dirent_cache*)desc;
    *count = cache_entry->data.filesize;
    return OBOS_STATUS_SUCCESS;
}
OBOS_WEAK obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead);
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
