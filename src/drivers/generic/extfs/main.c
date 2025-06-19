/*
 * drivers/generic/extfs/main.c
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * Abandon all hope ye who enter here
*/

#include <int.h>
#include <error.h>

#include <allocators/base.h>

#include <scheduler/thread.h>

#include <driver_interface/header.h>

#include <locks/mutex.h>

#include "structs.h"

LIST_GENERATE(ext_cache_list, ext_cache, node);
ext_cache_list EXT_CacheList;

OBOS_PAGEABLE_FUNCTION obos_status get_blk_size(dev_desc desc, size_t* blkSize)
{
    OBOS_UNUSED(desc);
    if (!blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *blkSize = 1;
    return OBOS_STATUS_SUCCESS;
}
OBOS_WEAK obos_status get_max_blk_count(dev_desc desc, size_t* count);

obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    ext_inode_handle *hnd = (void*)desc;
    if (!hnd || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // printf("%s: acquiring inode %d lock\n", __func__, hnd->ino);
    Core_MutexAcquire(&hnd->lock);
    obos_status status = ext_ino_read_blocks(hnd->cache, hnd->ino, blkOffset, blkCount, buf, nBlkRead);
    // printf("%s: releasing inode %d lock\n", __func__, hnd->ino);
    Core_MutexRelease(&hnd->lock);
    return status;
}

obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    ext_inode_handle *hnd = (void*)desc;
    if (!hnd || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    // printf("%s: acquiring inode %d lock\n", __func__, hnd->ino);
    Core_MutexAcquire(&hnd->lock);
    
    obos_status status = OBOS_STATUS_SUCCESS;

    size_t new_size = blkOffset+blkCount;
    status = ext_ino_resize(hnd->cache, hnd->ino, new_size, true);
    if (obos_is_error(status))
    {
        // printf("%s: releasing inode %d lock\n", __func__, hnd->ino);
        Core_MutexRelease(&hnd->lock);
        return status;
    }

    status = ext_ino_commit_blocks(hnd->cache, hnd->ino, blkOffset, blkCount);
    if (obos_is_error(status))
    {
        // printf("%s: releasing inode %d lock\n", __func__, hnd->ino);
        Core_MutexRelease(&hnd->lock);
        return status;
    }

    status = ext_ino_write_blocks(hnd->cache, hnd->ino, blkOffset, blkCount, buf, nBlkWritten);
    // printf("%s: releasing inode %d lock\n", __func__, hnd->ino);
    Core_MutexRelease(&hnd->lock);
    return status;
}

OBOS_WEAK obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* u), void* u);
OBOS_WEAK obos_status query_user_readable_name(dev_desc what, const char** name); // unrequired for fs drivers.
OBOS_PAGEABLE_FUNCTION obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    OBOS_UNUSED(what);
    OBOS_UNUSED(request);
    OBOS_UNUSED(argp);
    return OBOS_STATUS_INVALID_IOCTL;
}
void driver_cleanup_callback()
{
}
OBOS_WEAK obos_status query_path(dev_desc desc, const char** path);
OBOS_WEAK obos_status path_search(dev_desc* found, void*, const char* what);
OBOS_WEAK obos_status get_linked_path(dev_desc desc, const char** found);
OBOS_WEAK obos_status pmove_desc_to(void* vn, const char* path, const char* newpath, const char* name);
OBOS_WEAK obos_status pmk_file(dev_desc* newDesc, const char* parent_path, void* vn, const char* name, file_type type, driver_file_perm perm);
OBOS_WEAK obos_status premove_file(void* vn, const char* path);
OBOS_WEAK obos_status trunc_file(dev_desc desc, size_t newsize);
OBOS_WEAK obos_status set_file_perms(dev_desc desc, driver_file_perm newperm);
OBOS_WEAK obos_status get_file_perms(dev_desc desc, driver_file_perm *perm);
OBOS_WEAK obos_status get_file_type(dev_desc desc, file_type *type);
OBOS_WEAK obos_status list_dir(dev_desc dir, void* vn, iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata, const char* name), void* userdata);
OBOS_WEAK bool probe(void* vn);
OBOS_WEAK obos_status submit_irp(void* request);
OBOS_WEAK obos_status finalize_irp(void* request);
OBOS_WEAK obos_status ext_mount(void* vn, void* at);
OBOS_WEAK obos_status stat_fs_info(void *vn, drv_fs_info *info);
OBOS_WEAK obos_status vnode_search(void** vn_found, dev_desc desc, void* dev_vn);

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES|DRIVER_HEADER_FLAGS_NO_ENTRY|DRIVER_HEADER_DIRENT_CB_PATHS,
    .ftable = {
        .driver_cleanup_callback = driver_cleanup_callback,
        .ioctl = ioctl,
        .get_blk_size = get_blk_size,
        .get_max_blk_count = get_max_blk_count,
        .query_user_readable_name = query_user_readable_name,
        .foreach_device = foreach_device,
        .read_sync = read_sync,
        .write_sync = write_sync,
        .submit_irp = submit_irp,
        .finalize_irp = finalize_irp,

        .query_path = query_path,
        .path_search = path_search, // TODO: Implement
        .get_linked_path = get_linked_path,
        .pmove_desc_to = pmove_desc_to, // TODO: Implement
        .pmk_file = pmk_file, // TODO: Implement
        .premove_file = premove_file,
        .trunc_file = trunc_file, // TODO: Implement
        .get_file_perms = get_file_perms,
        .set_file_perms = set_file_perms,
        .get_file_type = get_file_type,
        .list_dir = list_dir,
        .vnode_search = vnode_search,
        .stat_fs_info = stat_fs_info,
        .probe = probe,
        .mount = nullptr,
    },
    .driverName = "EXT Driver",
};
