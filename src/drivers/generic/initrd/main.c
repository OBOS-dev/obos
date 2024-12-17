/*
 * drivers/generic/initrd/main.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>
#include <cmdline.h>

#include <stdarg.h>

#include <driver_interface/header.h>
#include <driver_interface/pci.h>

#include <stdint.h>
#include <utils/hashmap.h>

#include <uacpi_libc.h>

#include "name.h"
#include "ustar_hdr.h"
#include "parse.h"

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
OBOS_PAGEABLE_FUNCTION obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    OBOS_UNUSED(desc);
    OBOS_UNUSED(buf);
    OBOS_UNUSED(blkCount);
    OBOS_UNUSED(blkOffset);
    OBOS_UNUSED(nBlkWritten);
    return OBOS_STATUS_READ_ONLY;
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
struct hashmap* names;
void driver_cleanup_callback()
{
    FreeCache();
    if (names)
        hashmap_free(names);
}

OBOS_WEAK obos_status query_path(dev_desc desc, const char** path);
OBOS_WEAK obos_status path_search(dev_desc* found, void*, const char* what);
OBOS_WEAK obos_status get_linked_desc(dev_desc desc, dev_desc* found);
OBOS_PAGEABLE_FUNCTION obos_status move_desc_to(dev_desc desc, const char* where)
{
    OBOS_UNUSED(desc);
    OBOS_UNUSED(where);
    return OBOS_STATUS_READ_ONLY;
}
OBOS_PAGEABLE_FUNCTION obos_status mk_file(dev_desc* newDesc, dev_desc parent, void* vn, const char* name, file_type type) {
    OBOS_UNUSED(newDesc);
    OBOS_UNUSED(parent);
    OBOS_UNUSED(vn);
    OBOS_UNUSED(name);
    OBOS_UNUSED(type);
    return OBOS_STATUS_READ_ONLY;
}
OBOS_PAGEABLE_FUNCTION obos_status remove_file(dev_desc desc)
{
    OBOS_UNUSED(desc);
    return OBOS_STATUS_READ_ONLY;
}
OBOS_PAGEABLE_FUNCTION obos_status set_file_perms(dev_desc desc, driver_file_perm newperm)
{
    OBOS_UNUSED(desc);
    OBOS_UNUSED(newperm);
    return OBOS_STATUS_READ_ONLY;
}
OBOS_WEAK obos_status get_file_perms(dev_desc desc, driver_file_perm *perm);
OBOS_WEAK obos_status get_file_type(dev_desc desc, file_type *type);
OBOS_WEAK obos_status list_dir(dev_desc dir, void* unused, iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata), void* userdata);

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES|OBOS_STATUS_NO_ENTRY_POINT,
    .ftable = {
        .driver_cleanup_callback = driver_cleanup_callback,
        .ioctl = ioctl,
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
    },
    .driverName = INITRD_DRIVER_NAME
};

// dev_desc is simply a pointer to a ustar_hdr

OBOS_PAGEABLE_FUNCTION obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    const ustar_hdr* hdr = (ustar_hdr*)desc;
    if (!hdr || !count)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (hdr->type != AREGTYPE && hdr->type != REGTYPE)
        return OBOS_STATUS_NOT_A_FILE;
    *count = oct2bin(hdr->filesize, uacpi_strnlen(hdr->filesize, 12));;
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    const ustar_hdr* hdr = (ustar_hdr*)desc;
    if (!hdr || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!blkCount)
        return OBOS_STATUS_SUCCESS;
    if (hdr->type != AREGTYPE && hdr->type != REGTYPE)
        return OBOS_STATUS_NOT_A_FILE;
    size_t filesize = oct2bin(hdr->filesize, uacpi_strnlen(hdr->filesize, 12));;
    if (blkOffset >= filesize)
    {
        *nBlkRead = 0;
        return OBOS_STATUS_SUCCESS;
    }
    size_t nToRead = blkCount;
    if ((blkOffset + blkCount) >= filesize)
        nToRead = filesize - blkOffset;
    const char* data = (char*)(hdr) + 0x200;
    const char* iter = data+blkOffset;
    memcpy(buf, iter, nToRead);
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION static uint64_t hash(const void *item, uint64_t seed0, uint64_t seed1)
{
    const char* pck = item;
    return hashmap_sip(pck, uacpi_strnlen(pck, 100), seed0, seed1);
}
OBOS_PAGEABLE_FUNCTION static int cmp(const void *a, const void *b, void *udata)
{
    OBOS_UNUSED(udata);
    const char* pck1 = a;
    const char* pck2 = b;
    return uacpi_strncmp(pck1, pck2, 100);
}
OBOS_PAGEABLE_FUNCTION obos_status query_path(dev_desc desc, const char** path)
{
    const ustar_hdr* hdr = (ustar_hdr*)desc;
    if (!hdr || !path)
        return OBOS_STATUS_INVALID_ARGUMENT;
    const char* filepath = nullptr;
    if(uacpi_strnlen(hdr->filename, 100) == 100)
    {
        // Shit.
        if (!names)
        {
            names = hashmap_new_with_allocator(
                malloc, realloc, free,
                sizeof(char*), 
                64, 0, 0, 
                hash, cmp, 
                free,
                nullptr);
        }
        filepath = malloc(101);
        ((char*)filepath)[100] = 0;
        memcpy(((char*)filepath), hdr->filename, 100);
        hashmap_set(names, filepath);
    }
    else 
    {
        filepath = (const char*)&hdr->filename;    
    }
    *path = filepath;
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status get_linked_desc(dev_desc desc, dev_desc* found)
{
    const ustar_hdr* hdr = (ustar_hdr*)desc;
    if (!hdr || !found)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (hdr->type != LNKTYPE && hdr->type != SYMTYPE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    obos_status status = OBOS_STATUS_SUCCESS;
    *found = (dev_desc)GetFile(hdr->linked, &status);
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status path_search(dev_desc* found, void* unused, const char* what)
{
    OBOS_UNUSED(unused);
    if (!found || !what)
        return OBOS_STATUS_INVALID_ARGUMENT;
    obos_status status = OBOS_STATUS_SUCCESS;
    *found = (dev_desc)GetFile(what, &status);
    return status;
}
OBOS_PAGEABLE_FUNCTION obos_status get_file_perms(dev_desc desc, driver_file_perm *perm)
{
    const ustar_hdr* hdr = (ustar_hdr*)desc;
    if (!hdr || !perm)
        return OBOS_STATUS_INVALID_ARGUMENT;
    uint16_t filemode = oct2bin(hdr->filemode, uacpi_strnlen(hdr->filemode, 8));
    perm->group_read = filemode & FILEMODE_GROUP_READ;
    perm->owner_read = filemode & FILEMODE_OWNER_READ;
    perm->other_read = filemode & FILEMODE_OTHER_READ;
    perm->group_write = false;
    perm->owner_write = false;
    perm->other_write = false;
    perm->group_exec = filemode & FILEMODE_GROUP_EXEC;
    perm->owner_exec = filemode & FILEMODE_OWNER_EXEC;
    perm->other_exec = filemode & FILEMODE_OTHER_EXEC;
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status get_file_type(dev_desc desc, file_type *type)
{
    const ustar_hdr* hdr = (ustar_hdr*)desc;
    if (!hdr || !type)
        return OBOS_STATUS_INVALID_ARGUMENT;
    switch (hdr->type) {
        case AREGTYPE:
        case REGTYPE:
            *type = FILE_TYPE_REGULAR_FILE;
            break;
        case DIRTYPE:
            *type = FILE_TYPE_DIRECTORY;
            break;
        case LNKTYPE:
        case SYMTYPE:
            *type = FILE_TYPE_SYMBOLIC_LINK;
            break;
        default:
            return OBOS_STATUS_INTERNAL_ERROR;
    }
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status list_dir(dev_desc dir_, void* unused, iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata), void* userdata)
{
    OBOS_UNUSED(unused);
    const ustar_hdr* dir = (ustar_hdr*)dir_;
    if (!dir || !cb)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (dir_ != UINTPTR_MAX && dir->type != DIRTYPE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (dir_ != UINTPTR_MAX && uacpi_strnlen(dir->filename, 100) == 100)
        return OBOS_STATUS_INTERNAL_ERROR;
    const char* dir_filename = (const char*)&dir->filename;
    if (dir_ == UINTPTR_MAX)
        dir_filename = "";
    size_t dirnamelen = uacpi_strnlen(dir_filename, 100);
    size_t real_dirnamelen = dirnamelen;
    if (dir_filename[dirnamelen-1] == '/')
        dirnamelen--;
    // We don't get nice things like proper directory entries with the USTAR format.
    const ustar_hdr* hdr = (ustar_hdr*)OBOS_InitrdBinary;
    while (memcmp(hdr->magic, USTAR_MAGIC, 6))
    {
        size_t filesize = oct2bin(hdr->filesize, uacpi_strnlen(hdr->filesize, 12));
        size_t filename_len = uacpi_strnlen(hdr->filename, 100);
        if (!dirnamelen || (uacpi_strncmp(dir_filename, hdr->filename, dirnamelen) == 0  && real_dirnamelen != filename_len))
        {
            if (strchr(hdr->filename+real_dirnamelen, '/') == filename_len-real_dirnamelen)
                cb((dev_desc)hdr, 1, filesize, userdata);
        }
        size_t filesize_rounded = (filesize + 0x1ff) & ~0x1ff;
        hdr = (ustar_hdr*)(((uintptr_t)hdr) + filesize_rounded + 512);
    }
    return OBOS_STATUS_SUCCESS;
}
