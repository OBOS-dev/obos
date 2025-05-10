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

#include <vfs/irp.h>

#include <driver_interface/header.h>
#include <driver_interface/pci.h>

#include <uacpi_libc.h>

#include <driver_interface/driverId.h>

#include <allocators/base.h>

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

void driver_cleanup_callback()
{}

OBOS_WEAK obos_status query_path(dev_desc desc, const char** path);
OBOS_WEAK obos_status path_search(dev_desc* found, void*, const char* what);
OBOS_WEAK obos_status get_linked_desc(dev_desc desc, dev_desc* found);
OBOS_PAGEABLE_FUNCTION obos_status move_desc_to(dev_desc desc, dev_desc new_parent, const char* name)
{
    OBOS_UNUSED(desc);
    OBOS_UNUSED(new_parent && name);
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
OBOS_WEAK obos_status stat_fs_info(void *vn, drv_fs_info *info);

dev_desc irp_process_dryop(irp* req)
{
    dev_desc desc = req->desc;
    size_t blkCount = req->blkCount;
    size_t blkOffset = req->blkOffset;
    initrd_inode* ino = (void*)desc;
    if (!ino)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!blkCount)
        return OBOS_STATUS_SUCCESS;
    if (ino->type != FILE_TYPE_REGULAR_FILE)
        return OBOS_STATUS_NOT_A_FILE;
    size_t filesize = ino->filesize;
    if (blkOffset >= filesize)
    {
        req->nBlkRead = 0;
        return OBOS_STATUS_SUCCESS;
    }
    return OBOS_STATUS_SUCCESS;
}

OBOS_WEAK obos_status submit_irp(void* /* irp* */ request_)
{
    if (!request_)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irp* request = request_;
    if (request->op == IRP_WRITE)
        return OBOS_STATUS_INVALID_OPERATION;
    if (request->dryOp)
        request->status = irp_process_dryop(request);
    else
        request->status = read_sync(request->desc, request->buff, request->blkCount, request->blkOffset, &request->nBlkRead);
    request->evnt = nullptr;
    return OBOS_STATUS_SUCCESS;
}
OBOS_WEAK obos_status finalize_irp(void* /* irp* */ request_);

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES,
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
        .get_linked_desc = get_linked_desc,
        .move_desc_to = move_desc_to,
        .mk_file = mk_file,
        .remove_file = remove_file,
        .get_file_perms = get_file_perms,
        .set_file_perms = set_file_perms,
        .get_file_type = get_file_type,
        .list_dir = list_dir,
        .stat_fs_info = stat_fs_info,
    },
    .driverName = INITRD_DRIVER_NAME,
    .version = 1,
};

initrd_inode* InitrdRoot;
size_t CurrentInodeNumber = 1;

static int64_t strrfind(const char* str, char ch)
{
    int64_t i = strlen(str);
    for (; i >= 0; i--)
        if (str[i] == ch)
           return i;
    return -1;
}

initrd_inode* create_inode_boot(const ustar_hdr* hdr)
{
    initrd_inode* ino = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(initrd_inode), nullptr);
    ino->ino = CurrentInodeNumber++;
    ino->path_len = strnlen(hdr->filename, 100);
    ino->path_size = ino->path_len;
    ino->path = Allocate(OBOS_KernelAllocator, ino->path_len+1, nullptr);
    memcpy(ino->path, hdr->filename, ino->path_len);
    ino->path[ino->path_len] = 0;
    ino->persistent = true;
    // Get our parent.
    // Also, seperate the path from the basename.
    while (ino->path[--ino->path_len] == '/')
        ino->path[ino->path_len] = 0;
    int64_t index = strrfind(ino->path, '/')+1;
    ino->name_len = ino->path_len - index+1;
    ino->name_size = ino->name_len;
    ino->name = Allocate(OBOS_KernelAllocator, ino->name_len+1, nullptr);
    memcpy(ino->name, ino->path+index, ino->name_len);
    ino->name[ino->name_len] = 0;
    ino->filesize = oct2bin(hdr->filesize, uacpi_strnlen(hdr->filesize, 12));
    ino->data = Allocate(OBOS_KernelAllocator, ino->filesize, nullptr);
    memcpy(ino->data, (char*)hdr + 512, ino->filesize);
    switch (hdr->type) {
        case AREGTYPE:
        case REGTYPE:
            ino->type = FILE_TYPE_REGULAR_FILE;
            break;
        case DIRTYPE:
            ino->type = FILE_TYPE_DIRECTORY;
            break;
        case LNKTYPE:
        case SYMTYPE:
            ino->type = FILE_TYPE_SYMBOLIC_LINK;
            break;
        default:
            OBOS_ENSURE(!"Unrecognized header type");
    }
    uint16_t filemode = oct2bin(hdr->filemode, uacpi_strnlen(hdr->filemode, 8));
    ino->perm.group_read = filemode & FILEMODE_GROUP_READ;
    ino->perm.owner_read = filemode & FILEMODE_OWNER_READ;
    ino->perm.other_read = filemode & FILEMODE_OTHER_READ;
    ino->perm.group_write = filemode & FILEMODE_GROUP_WRITE;
    ino->perm.owner_write = filemode & FILEMODE_OWNER_WRITE;
    ino->perm.other_write = filemode & FILEMODE_OTHER_WRITE;
    ino->perm.group_exec = filemode & FILEMODE_GROUP_EXEC;
    ino->perm.owner_exec = filemode & FILEMODE_OWNER_EXEC;
    ino->perm.other_exec = filemode & FILEMODE_OTHER_EXEC;
    return ino;
}
driver_init_status OBOS_DriverEntry(driver_id* this)
{
    (void)this;

    // Parse the initrd, and make the initrd_inode tree.

    InitrdRoot = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(initrd_inode), nullptr);
    InitrdRoot->filesize = 0;
    InitrdRoot->name = "";
    InitrdRoot->path = "";
    InitrdRoot->type = FILE_TYPE_DIRECTORY;
    InitrdRoot->persistent = true;
    
    InitrdRoot->perm.owner_exec = true;
    InitrdRoot->perm.group_exec = true;
    InitrdRoot->perm.other_exec = false;

    InitrdRoot->perm.owner_read = true;
    InitrdRoot->perm.group_read = true;
    InitrdRoot->perm.other_read = false;
    
    InitrdRoot->perm.owner_write = true;
    InitrdRoot->perm.group_write = true;
    InitrdRoot->perm.other_write = false;

    InitrdRoot->ino = CurrentInodeNumber++;

    const ustar_hdr* hdr = (ustar_hdr*)OBOS_InitrdBinary;
    while (memcmp(hdr->magic, USTAR_MAGIC, 6))
    {
        if (hdr->type == DIRTYPE)
        {
            // Check if it exists first.
            initrd_inode* us = DirentLookupFrom(hdr->filename, InitrdRoot);
            if (us)
                continue;
        }
        initrd_inode* ino = create_inode_boot(hdr);

        // Get our parent.
        int64_t index = strrfind(ino->path, '/');
        if (index == -1)
            index = 0;
        char presv = ino->path[index];
        ino->path[index] = 0;
        ino->parent = DirentLookupFrom(ino->path /* really the 'dirname' */, InitrdRoot);
        ino->path[index] = presv;

        if (!ino->parent)
        {
            // Create all parent directories
            char* iter = ino->path;
            char* end = ino->path;
            while (iter < end)
            {
                size_t off = strchr(iter, '/');
                presv = iter[off];
                iter[off] = 0;
                if (DirentLookupFrom(ino->path, InitrdRoot))
                    goto down;

                const ustar_hdr *sub_hdr = GetFile(ino->path, nullptr);
                OBOS_ASSERT(sub_hdr);
                
                initrd_inode* sub_ino = create_inode_boot(sub_hdr);
                if (!sub_ino->parent->children.head)
                    sub_ino->parent->children.head = sub_ino;
                if (sub_ino->parent->children.tail)
                    sub_ino->parent->children.tail->next = sub_ino;
                sub_ino->prev = sub_ino->parent->children.tail;
                sub_ino->parent->children.tail = sub_ino;
                sub_ino->parent->children.nChildren++;
                sub_ino->parent = sub_ino->parent;
                
                ino->parent = sub_ino;

                down:
                iter[off] = presv;
                iter += off;
            }
        }
        if (!ino->parent)
            ino->parent = InitrdRoot;
        if (!ino->parent->children.head)
            ino->parent->children.head = ino;
        if (ino->parent->children.tail)
            ino->parent->children.tail->next = ino;
        ino->prev = ino->parent->children.tail;
        ino->parent->children.tail = ino;
        ino->parent->children.nChildren++;

        size_t filesize_rounded = (ino->filesize + 0x1ff) & ~0x1ff;
        hdr = (ustar_hdr*)(((uintptr_t)hdr) + filesize_rounded + 512);
    }    

    return (driver_init_status){.status=OBOS_STATUS_SUCCESS,.fatal=false};
}

// dev_desc is simply a pointer to a ustar_hdr

OBOS_PAGEABLE_FUNCTION obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    initrd_inode* inode = (void*)desc;
    if (!inode || !count)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (inode->type != FILE_TYPE_REGULAR_FILE)
        return OBOS_STATUS_NOT_A_FILE;
    *count = inode->filesize;
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    initrd_inode* inode = (void*)desc;
    if (!inode || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!blkCount)
        return OBOS_STATUS_SUCCESS;
    if (inode->type != FILE_TYPE_REGULAR_FILE)
        return OBOS_STATUS_NOT_A_FILE;
    if (blkOffset >= inode->filesize)
    {
        *nBlkRead = 0;
        return OBOS_STATUS_SUCCESS;
    }
    size_t nToRead = blkCount;
    if ((blkOffset + blkCount) >= inode->filesize)
        nToRead = inode->filesize - blkOffset;
    const char* iter = inode->data+blkOffset;
    memcpy(buf, iter, nToRead);
    return OBOS_STATUS_SUCCESS;
}

OBOS_PAGEABLE_FUNCTION obos_status query_path(dev_desc desc, const char** path)
{
    initrd_inode* inode = (void*)desc;
    if (!inode || !path)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *path = inode->path;
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status get_linked_desc(dev_desc desc, dev_desc* found)
{    
    OBOS_UNUSED(desc && found);
    return OBOS_STATUS_UNIMPLEMENTED;
}
OBOS_PAGEABLE_FUNCTION obos_status path_search(dev_desc* found, void* unused, const char* what)
{
    OBOS_UNUSED(unused);
    if (!found || !what)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *found = (dev_desc)DirentLookupFrom(what, InitrdRoot);
    return *found ? OBOS_STATUS_SUCCESS : OBOS_STATUS_NOT_FOUND;
}
OBOS_PAGEABLE_FUNCTION obos_status get_file_perms(dev_desc desc, driver_file_perm *perm)
{
    initrd_inode* inode = (void*)desc;
    if (!inode || !perm)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *perm = inode->perm;
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status get_file_type(dev_desc desc, file_type *type)
{
    if (!type || !desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    initrd_inode* inode = (void*)desc;
    *type = inode->type;
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status list_dir(dev_desc dir_, void* unused, iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata), void* userdata)
{
    OBOS_UNUSED(unused);
    if (!dir_)
        return OBOS_STATUS_INVALID_ARGUMENT;
    initrd_inode* dir = dir_ == UINTPTR_MAX ? InitrdRoot : (void*)dir_;
    for (initrd_inode* ino = dir->children.head; ino; )
    {
        if (cb((dev_desc)ino, 1, ino->filesize, userdata) == ITERATE_DECISION_STOP)
            break;
        ino = ino->next;
    }
    return OBOS_STATUS_SUCCESS;
}

static iterate_decision cb(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata)
{
    OBOS_UNUSED(blkSize);
    OBOS_UNUSED(blkCount);

    size_t* const fileCount = userdata;
    (*fileCount)++;

    initrd_inode* ino = (void*)desc;
    if (ino->type == FILE_TYPE_DIRECTORY)
        list_dir(desc, nullptr, cb, userdata);
    return ITERATE_DECISION_CONTINUE;
}

obos_status stat_fs_info(void *vn, drv_fs_info *info)
{
    OBOS_UNUSED(vn);
    static size_t fileCount = SIZE_MAX;
    if (fileCount == SIZE_MAX)
    {
        fileCount = 0;
        list_dir(UINTPTR_MAX, vn, cb, &fileCount);
    }
    info->partBlockSize = 1;
    info->fsBlockSize = 1;
    info->availableFiles = 0;
    info->freeBlocks = 0;
    info->fileCount = fileCount;
    info->szFs = OBOS_InitrdSize;
    info->flags = FS_FLAGS_RDONLY;
    // TODO: Is there a proper value for this?
    info->nameMax = 100;
    return OBOS_STATUS_SUCCESS;
}
