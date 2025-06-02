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
    return ext_ino_read_blocks(hnd->cache, hnd->ino, blkOffset, blkCount, buf, nBlkRead);
}

OBOS_WEAK obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);
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
OBOS_WEAK obos_status move_desc_to(dev_desc desc, dev_desc new_parent, const char* name);
OBOS_WEAK obos_status mk_file(dev_desc* newDesc, dev_desc parent, void* vn, const char* name, file_type type, driver_file_perm perm);
OBOS_WEAK obos_status remove_file(dev_desc desc);
OBOS_WEAK obos_status trunc_file(dev_desc desc, size_t newsize);
OBOS_WEAK obos_status set_file_perms(dev_desc desc, driver_file_perm newperm);
OBOS_WEAK obos_status get_file_perms(dev_desc desc, driver_file_perm *perm);
OBOS_WEAK obos_status get_file_type(dev_desc desc, file_type *type);
OBOS_WEAK obos_status list_dir(dev_desc dir, void* vn, iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata), void* userdata);
OBOS_WEAK bool probe(void* vn);
OBOS_WEAK obos_status submit_irp(void* request);
OBOS_WEAK obos_status finalize_irp(void* request);
OBOS_WEAK obos_status ext_mount(void* vn, void* at);
OBOS_WEAK obos_status stat_fs_info(void *vn, drv_fs_info *info);

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
        .path_search = path_search,
        .get_linked_path = get_linked_path,
        .move_desc_to = move_desc_to,
        .mk_file = mk_file,
        .remove_file = remove_file,
        .trunc_file = trunc_file,
        .get_file_perms = get_file_perms,
        .set_file_perms = set_file_perms,
        .get_file_type = get_file_type,
        .list_dir = list_dir,
        .stat_fs_info = stat_fs_info,
        .probe = probe,
        .mount = ext_mount,
    },
    .driverName = "EXT Driver",
};
