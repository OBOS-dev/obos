/*
 * drivers/generic/slowfat/main.c
 *
 * Copyright (c) 2024 Omar Berrow
 *
 * Abandon all hope ye who enter here
*/

#include <int.h>

#include <allocators/base.h>

#include <scheduler/thread.h>

#include <driver_interface/header.h>


OBOS_PAGEABLE_FUNCTION obos_status get_blk_size(dev_desc desc, size_t* blkSize)
{
    OBOS_UNUSED(desc);
    if (!blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *blkSize = 1;
    return OBOS_STATUS_SUCCESS;
}
OBOS_WEAK obos_status get_max_blk_count(dev_desc desc, size_t* count);
OBOS_WEAK obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead);
OBOS_WEAK obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);
OBOS_WEAK obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* u), void* u);
OBOS_WEAK obos_status query_user_readable_name(dev_desc what, const char** name); // unrequired for fs drivers.
OBOS_PAGEABLE_FUNCTION obos_status ioctl_var(size_t nParameters, uint64_t request, va_list list)
{
    OBOS_UNUSED(nParameters);
    OBOS_UNUSED(request);
    OBOS_UNUSED(list);
    return OBOS_STATUS_INVALID_IOCTL; // we don't support any
}
OBOS_PAGEABLE_FUNCTION obos_status ioctl(size_t nParameters, uint64_t request, ...)
{
    va_list list;
    va_start(list, request);
    obos_status status = ioctl_var(nParameters, request, list);
    va_end(list);
    return status;
}
void driver_cleanup_callback()
{
}
OBOS_WEAK obos_status query_path(dev_desc desc, const char** path);
OBOS_WEAK obos_status path_search(dev_desc* found, void*, const char* what);
OBOS_WEAK obos_status get_linked_desc(dev_desc desc, dev_desc* found);
OBOS_WEAK obos_status move_desc_to(dev_desc desc, const char* where);
OBOS_WEAK obos_status mk_file(dev_desc* newDesc, dev_desc parent, const char* name, file_type type);
OBOS_WEAK obos_status remove_file(dev_desc desc);
OBOS_WEAK obos_status set_file_perms(dev_desc desc, driver_file_perm newperm);
OBOS_WEAK obos_status get_file_perms(dev_desc desc, driver_file_perm *perm);
OBOS_WEAK obos_status get_file_type(dev_desc desc, file_type *type);
OBOS_WEAK obos_status list_dir(dev_desc dir, void* vn, iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata), void* userdata);
OBOS_WEAK bool probe(void* vn);

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES,
    .ftable = {
        .driver_cleanup_callback = driver_cleanup_callback,
        .ioctl = ioctl,
        .ioctl_var = ioctl_var,
        .get_blk_size = get_blk_size,
        .get_max_blk_count = get_max_blk_count,
        .query_user_readable_name = query_user_readable_name,
        .foreach_device = foreach_device,
        .read_sync = read_sync,
        .write_sync = write_sync,

        .query_path = query_path,
        .path_search = path_search,
        .get_linked_desc = get_linked_desc,
        .move_desc_to = move_desc_to,
        .mk_file = mk_file,
        .remove_file = remove_file,
        .get_file_perms = get_file_perms,
        .set_file_perms = set_file_perms,
        .get_file_type = get_file_type,
        .list_dir = list_dir,
        .probe = probe,
    },
    .driverName = "FAT Driver",
};
allocator_info* FATAllocator = nullptr;
void OBOS_DriverEntry()
{
    FATAllocator = OBOS_NonPagedPoolAllocator;
    Core_ExitCurrentThread();
}