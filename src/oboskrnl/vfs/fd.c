/*
 * oboskrnl/vfs/fd.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <error.h>
#include <partition.h>

#include <vfs/vnode.h>
#include <vfs/alloc.h>
#include <vfs/fd.h>
#include <vfs/limits.h>
#include <vfs/dirent.h>
#include <vfs/pagecache.h>
#include <vfs/mount.h>

#include <mm/swap.h>

#include <scheduler/schedule.h>
#include <scheduler/process.h>
#include <scheduler/thread.h>

#include <utils/list.h>

#include <locks/mutex.h>

#include <driver_interface/driverId.h>
#include <driver_interface/header.h>

// if depth == zero, it will search all nodes starting at 'from'
// otherwise, it will only search 'depth' nodes

static bool is_eof(vnode* vn, size_t off)
{
    return off > vn->filesize;
}
obos_status Vfs_FdOpen(fd* const desc, const char* path, uint32_t oflags) 
{
    dirent* ent = VfsH_DirentLookup(path);
    if (!ent)
        return OBOS_STATUS_NOT_FOUND;
    return Vfs_FdOpenDirent(desc, ent, oflags);
}
obos_status Vfs_FdOpenDirent(fd* const desc, dirent* ent, uint32_t oflags)
{
    if (!desc || !ent)
        return OBOS_STATUS_INVALID_ARGUMENT;
    return Vfs_FdOpenVnode(desc, ent->vnode, oflags);
}
OBOS_EXPORT obos_status Vfs_FdOpenVnode(fd* const desc, void* vn, uint32_t oflags)
{
    if (!desc || !vn)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (desc->flags & FD_FLAGS_OPEN)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    if ((~oflags & FD_OFLAGS_WRITE) && (~oflags & FD_OFLAGS_READ))
        return OBOS_STATUS_INVALID_ARGUMENT;
    vnode* vnode = vn;
    OBOS_ASSERT(vnode);
    if (vnode->vtype == VNODE_TYPE_DIR)
        return OBOS_STATUS_NOT_A_FILE;
    desc->vn = vnode;
    desc->flags |= FD_FLAGS_OPEN;
    desc->flags |= FD_FLAGS_READ;
    desc->flags |= FD_FLAGS_WRITE;
    if (desc->vn->owner_uid == Core_GetCurrentThread()->proc->currentUID)
    {
        // We have owner perms.
        struct vnode* const vn = desc->vn;
        if (!vn->perm.owner_read)
            desc->flags &= FD_FLAGS_READ;
        if (!vn->perm.owner_write)
            desc->flags &= FD_FLAGS_READ;
    }
    else if (desc->vn->group_uid == Core_GetCurrentThread()->proc->currentGID)
    {
        // We have group perms.
        struct vnode* const vn = desc->vn;
        if (!vn->perm.group_read)
            desc->flags &= FD_FLAGS_READ;
        if (!vn->perm.group_write)
            desc->flags &= FD_FLAGS_READ;
    }
    else
    {
        // We have other perms.
        struct vnode* const vn = desc->vn;
        if (!vn->perm.other_read)
            desc->flags &= FD_FLAGS_READ;
        if (!vn->perm.other_write)
            desc->flags &= FD_FLAGS_READ;
    }
    if (~oflags & FD_OFLAGS_READ)
        desc->flags &= ~FD_FLAGS_READ;
    if (~oflags & FD_OFLAGS_WRITE)
        desc->flags &= ~FD_FLAGS_WRITE;
    if (oflags & FD_OFLAGS_UNCACHED)
        desc->flags |= FD_FLAGS_UNCACHED;
    if (oflags & FD_OFLAGS_NOEXEC)
        desc->flags |= FD_FLAGS_NOEXEC;
    if (vnode->vtype == VNODE_TYPE_CHR)
        desc->flags |= FD_FLAGS_UNCACHED;
    desc->vn->refs++;
    LIST_APPEND(fd_list, &desc->vn->opened, desc);
    desc->flags |= FD_FLAGS_OPEN;
    return OBOS_STATUS_SUCCESS;
}
static obos_status do_uncached_write(fd* desc, const void* from, size_t nBytes, size_t* nWritten_)
{
    mount* const point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
    const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (desc->vn->vtype == VNODE_TYPE_CHR || desc->vn->vtype == VNODE_TYPE_BLK)
        driver = &desc->vn->un.device->driver->header;
    size_t blkSize = 0;
    driver->ftable.get_blk_size(desc->vn->desc, &blkSize);
    if (nBytes % blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    nBytes /= blkSize;
    const size_t base_offset = desc->vn->flags & VFLAGS_PARTITION ? desc->vn->partitions[0].off : 0;
    const uintptr_t offset = (desc->offset + base_offset) / blkSize;
    if (!VfsH_LockMountpoint(point))
        return OBOS_STATUS_ABORTED;
    obos_status status = driver->ftable.write_sync(desc->vn->desc, from, nBytes, offset, nWritten_);
    VfsH_UnlockMountpoint(point);
    if (obos_expect(obos_is_error(status) == true, 0))
        return status;
    if (nWritten_)
        *nWritten_ *= blkSize;
    return OBOS_STATUS_SUCCESS;
}
obos_status Vfs_FdWrite(fd* desc, const void* buf, size_t nBytes, size_t* nWritten)
{
    if (!desc || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    if (!nBytes)
        return OBOS_STATUS_SUCCESS;
    if (is_eof(desc->vn, desc->offset))
        return OBOS_STATUS_EOF;
    if (!(desc->flags & FD_FLAGS_WRITE))
        return OBOS_STATUS_ACCESS_DENIED;
    obos_status status = OBOS_STATUS_SUCCESS;
    if (desc->flags & FD_FLAGS_UNCACHED)
    {
        // Keep it nice and simple, and just do an uncached write on the file.

        status = do_uncached_write(desc, buf, nBytes, nWritten);
    }
    else 
    {
        mount* const point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
        if (!VfsH_LockMountpoint(point))
            return OBOS_STATUS_ABORTED;

        size_t bytesLeft = nBytes;
        size_t offset = desc->offset;
        for (size_t i = 0; bytesLeft; i += OBOS_PAGE_SIZE)
        {
            uint8_t* ent = VfsH_PageCacheGetEntry(desc->vn, desc->offset+i, false) + (offset % OBOS_PAGE_SIZE);
            if (!ent)
            {
                status = OBOS_STATUS_INTERNAL_ERROR;
                goto done;
            }
            memcpy(ent, (const uint8_t*)buf+i, OBOS_MIN(bytesLeft, OBOS_PAGE_SIZE - (offset % OBOS_PAGE_SIZE)));
            if (bytesLeft > OBOS_PAGE_SIZE)
                bytesLeft -= OBOS_PAGE_SIZE;
            else
                bytesLeft = 0;
            offset += OBOS_PAGE_SIZE;
        }
        done:
        VfsH_UnlockMountpoint(point);

        if (nWritten)
            *nWritten = nBytes;
    }
    if (obos_expect(obos_is_success(status), 1))
    {
        if (nBytes > (desc->vn->filesize - desc->offset) && desc->vn->vtype == VNODE_TYPE_REG)
            desc->vn->filesize += (nBytes-(desc->vn->filesize - desc->offset)); // add the difference to the file size
        Vfs_FdSeek(desc, nBytes, SEEK_CUR);
    }
    return status;
}
static obos_status do_uncached_read(fd* desc, void* into, size_t nBytes, size_t* nRead_)
{
    mount* const point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
    const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (desc->vn->vtype == VNODE_TYPE_CHR || desc->vn->vtype == VNODE_TYPE_BLK)
        driver = &desc->vn->un.device->driver->header;
    size_t blkSize = 0;
    driver->ftable.get_blk_size(desc->vn->desc, &blkSize);
    if (nBytes % blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    nBytes /= blkSize;
    const size_t base_offset = desc->vn->flags & VFLAGS_PARTITION ? desc->vn->partitions[0].off : 0;
    const uintptr_t offset = (desc->offset+base_offset) / blkSize;
    if (!VfsH_LockMountpoint(point))
        return OBOS_STATUS_ABORTED;
    obos_status status = driver->ftable.read_sync(desc->vn->desc, into, nBytes, offset, nRead_);
    VfsH_UnlockMountpoint(point);
    if (obos_expect(obos_is_error(status) == true, 0))
        return status;
    if (nRead_)
        *nRead_ *= blkSize;
    return OBOS_STATUS_SUCCESS;
}
obos_status Vfs_FdRead(fd* desc, void* buf, size_t nBytes, size_t* nRead)
{
    if (!desc || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    if (!nBytes)
        return OBOS_STATUS_SUCCESS;
    if (is_eof(desc->vn, desc->offset))
        return OBOS_STATUS_EOF;
    if (!(desc->flags & FD_FLAGS_READ))
        return OBOS_STATUS_ACCESS_DENIED;
    if (nBytes > (desc->vn->filesize - desc->offset) && desc->vn->vtype != VNODE_TYPE_CHR)
        nBytes = desc->vn->filesize - desc->offset; // truncate size to the space we have left in the file.
    obos_status status = OBOS_STATUS_SUCCESS;
    if (desc->flags & FD_FLAGS_UNCACHED)
    {
        // Keep it nice and simple, and just do an uncached read on the file.

        status = do_uncached_read(desc, buf, nBytes, nRead);
    }
    else 
    {
        mount* const point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
        // const size_t base_offset = desc->vn->flags & VFLAGS_PARTITION ? desc->vn->partitions[0].off : 0;
        if (!VfsH_LockMountpoint(point))
            return OBOS_STATUS_ABORTED;
        
        size_t bytesLeft = nBytes;
        size_t offset = desc->offset;
        // for (volatile bool b = (nBytes==0x2918); b; );
        while (bytesLeft)
        {
            const uint8_t* ent = VfsH_PageCacheGetEntry(desc->vn, offset, false) + (offset % OBOS_PAGE_SIZE);
            if (!ent)
            {
                status = OBOS_STATUS_INTERNAL_ERROR;
                goto done;
            }
            memcpy((uint8_t*)buf+(offset-desc->offset), ent, OBOS_MIN(bytesLeft, OBOS_PAGE_SIZE - (offset % OBOS_PAGE_SIZE)));
            if (bytesLeft > OBOS_PAGE_SIZE)
                bytesLeft -= OBOS_PAGE_SIZE;
            else
                bytesLeft = 0;
            offset += (OBOS_PAGE_SIZE-(offset%OBOS_PAGE_SIZE));
        }
        done:
        VfsH_UnlockMountpoint(point);

        if (nRead)
            *nRead = nBytes;
    }
    if (obos_expect(obos_is_success(status), 1))
        Vfs_FdSeek(desc, nBytes, SEEK_CUR);
    return status;
}
obos_status Vfs_FdSeek(fd* desc, off_t off, whence_t whence)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    if (desc->vn->vtype == VNODE_TYPE_FIFO)
        return OBOS_STATUS_INVALID_OPERATION;
    size_t finalOff = 0;
    mount* const point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
    const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (desc->vn->vtype == VNODE_TYPE_CHR || desc->vn->vtype == VNODE_TYPE_BLK)
        driver = &desc->vn->un.device->driver->header;
    size_t blkSize = 0;
    driver->ftable.get_blk_size(desc->vn->desc, &blkSize);
    switch (whence)
    {
        case SEEK_SET:
            if (off % blkSize)
                return OBOS_STATUS_INVALID_ARGUMENT;
            finalOff = off;
            break;
        case SEEK_END:
            if (off % blkSize)
                return OBOS_STATUS_INVALID_ARGUMENT;
            finalOff = (off_t)desc->vn->filesize - 1 + off;
            finalOff -= finalOff % blkSize;
            break;
        case SEEK_CUR:
            if (off % blkSize)
                return OBOS_STATUS_INVALID_ARGUMENT;
            finalOff = (off_t)desc->offset + off;
            finalOff -= finalOff % blkSize;
            break;
    }
    if (is_eof(desc->vn, finalOff))
        return OBOS_STATUS_EOF;
    desc->offset = finalOff;
    return OBOS_STATUS_SUCCESS;
}
uoff_t Vfs_FdTellOff(const fd* desc)
{
    if (desc && desc->vn && desc->vn->vtype != VNODE_TYPE_FIFO)
        return desc->offset;
    return (uoff_t)(-1);
}
size_t Vfs_FdGetBlkSz(const fd* desc)
{
    if (!desc)
        return (size_t)-1;
    mount* const point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
    const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (desc->vn->vtype == VNODE_TYPE_CHR || desc->vn->vtype == VNODE_TYPE_BLK)
        driver = &desc->vn->un.device->driver->header;
    size_t blkSize = 0;
    driver->ftable.get_blk_size(desc->vn->desc, &blkSize);
    return blkSize;
}
obos_status Vfs_FdEOF(const fd* desc)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    return is_eof(desc->vn, desc->offset) ? OBOS_STATUS_EOF : OBOS_STATUS_SUCCESS;
}
vnode* Vfs_FdGetVnode(fd* desc)
{
    return desc ? desc->vn : nullptr;
}
obos_status Vfs_FdIoctl(fd* desc, uint64_t request, void* argp)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    if (desc->vn->vtype != VNODE_TYPE_BLK && desc->vn->vtype != VNODE_TYPE_CHR)
        return OBOS_STATUS_INVALID_IOCTL;
    return desc->vn->un.device->driver->header.ftable.ioctl(desc->vn->un.device->desc, request, argp);
}
obos_status Vfs_FdFlush(fd* desc)
{
    if (!desc)
        return OBOS_STATUS_SUCCESS;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (desc->flags & FD_FLAGS_UNCACHED)
        return OBOS_STATUS_INVALID_OPERATION;
    mount* const point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
    if (!VfsH_LockMountpoint(point))
        return OBOS_STATUS_ABORTED;
    Mm_WakePageWriter(false);
    VfsH_UnlockMountpoint(point);
    return OBOS_STATUS_SUCCESS;
}
obos_status Vfs_FdClose(fd* desc)
{
    if (!desc)
        return OBOS_STATUS_SUCCESS;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_INVALID_ARGUMENT;
    Vfs_FdFlush(desc);
    mount* const point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
    if (!VfsH_LockMountpoint(point))
        return OBOS_STATUS_ABORTED;
    vnode* vn = desc->vn;
    LIST_REMOVE(fd_list, &desc->vn->opened, desc);
    vn->refs--;
    desc->flags &= ~FD_FLAGS_OPEN;
    VfsH_UnlockMountpoint(point);
    return OBOS_STATUS_SUCCESS;
}
LIST_GENERATE_INTERNAL(fd_list, struct fd, node, OBOS_EXPORT);
