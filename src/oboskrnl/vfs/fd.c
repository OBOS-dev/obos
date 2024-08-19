/*
 * oboskrnl/vfs/fd.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <vfs/vnode.h>
#include <vfs/alloc.h>
#include <vfs/fd.h>
#include <vfs/limits.h>
#include <vfs/dirent.h>
#include <vfs/pagecache.h>
#include <vfs/mount.h>

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
    if (vn->vtype != VNODE_TYPE_REG)
        return false;
    return off >= vn->filesize;
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
    if (desc->flags & FD_FLAGS_OPEN)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    OBOS_ASSERT(ent->vnode);
    if (ent->vnode->vtype == VNODE_TYPE_DIR)
        return OBOS_STATUS_NOT_A_FILE;
    desc->vn = ent->vnode;
    desc->flags |= FD_FLAGS_OPEN;
    desc->flags |= FD_FLAGS_READ;
    desc->flags |= FD_FLAGS_WRITE;
    if (desc->vn->owner_uid == Core_GetCurrentThread()->proc->currentUID)
    {
        // We have owner perms.
        vnode* const vn = desc->vn;
        if (!vn->perm.owner_read)
            desc->flags &= FD_FLAGS_READ;
        if (!vn->perm.owner_write)
            desc->flags &= FD_FLAGS_READ;
    }
    else if (desc->vn->group_uid == Core_GetCurrentThread()->proc->currentGID)
    {
        // We have group perms.
        vnode* const vn = desc->vn;
        if (!vn->perm.group_read)
            desc->flags &= FD_FLAGS_READ;
        if (!vn->perm.group_write)
            desc->flags &= FD_FLAGS_READ;
    }
    else
    {
        // We have other perms.
        vnode* const vn = desc->vn;
        if (!vn->perm.other_read)
            desc->flags &= FD_FLAGS_READ;
        if (!vn->perm.other_write)
            desc->flags &= FD_FLAGS_READ;
    }
    if (oflags & FD_OFLAGS_READ_ONLY)
        desc->flags &= ~FD_FLAGS_WRITE;
    if (oflags & FD_OFLAGS_UNCACHED)
        desc->flags |= FD_FLAGS_UNCACHED;
    desc->vn->refs++;
    desc->flags |= FD_FLAGS_OPEN;
    return OBOS_STATUS_SUCCESS;
}
static obos_status do_uncached_write(fd* desc, const void* from, size_t nBytes, size_t* nWritten_)
{
    const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &desc->vn->mount_point->fs_driver->driver->header : nullptr;
    if (desc->vn->vtype == VNODE_TYPE_CHR || desc->vn->vtype == VNODE_TYPE_BLK)
        driver = &desc->vn->un.device->driver->header;
    size_t blkSize = 0;
    driver->ftable.get_blk_size(desc->vn->desc, &blkSize);
    if (nBytes % blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    nBytes /= blkSize;
    const uintptr_t offset = desc->offset / blkSize;
    obos_status status = driver->ftable.write_sync(desc->vn->desc, from, nBytes, offset, nWritten_);
    if (obos_expect(obos_is_error(status) == true, 0))
        return status;
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
    if (nBytes > (desc->vn->filesize - desc->offset) && desc->vn->vtype == VNODE_TYPE_REG)
        desc->vn->filesize += (nBytes-(desc->vn->filesize - desc->offset)); // add the difference to the file size
    obos_status status = OBOS_STATUS_SUCCESS;
    if (desc->flags & FD_FLAGS_UNCACHED)
    {
        // Keep it nice and simple, and just do an uncached write on the file.

        status = do_uncached_write(desc, buf, nBytes, nWritten);
    }
    else 
    {
        pagecache_dirty_region* dirty = VfsH_PCDirtyRegionCreate(&desc->vn->pagecache, desc->offset, nBytes);
        OBOS_ASSERT(obos_expect(dirty != nullptr, 0));
        Core_MutexAcquire(&dirty->lock);
        if (desc->vn->pagecache.sz <= desc->offset)
            VfsH_PageCacheResize(&desc->vn->pagecache, desc->vn, desc->offset + nBytes);
        memcpy(desc->vn->pagecache.data + desc->offset, buf, nBytes);
        Core_MutexRelease(&dirty->lock);

        if (nWritten)
            *nWritten = nBytes;
    }
    if (obos_expect(obos_is_success(status), 1))
        Vfs_FdSeek(desc, nBytes, SEEK_CUR);
    return status;
}
static obos_status do_uncached_read(fd* desc, void* into, size_t nBytes, size_t* nRead_)
{
    const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &desc->vn->mount_point->fs_driver->driver->header : nullptr;
    if (desc->vn->vtype == VNODE_TYPE_CHR || desc->vn->vtype == VNODE_TYPE_BLK)
        driver = &desc->vn->un.device->driver->header;
    size_t blkSize = 0;
    driver->ftable.get_blk_size(desc->vn->desc, &blkSize);
    if (nBytes % blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    nBytes /= blkSize;
    const uintptr_t offset = desc->offset / blkSize;
    obos_status status = driver->ftable.read_sync(desc->vn->desc, into, nBytes, offset, nRead_);
    if (obos_expect(obos_is_error(status) == true, 0))
        return status;
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
    if (nBytes > (desc->vn->filesize - desc->offset) && desc->vn->vtype == VNODE_TYPE_REG)
        nBytes = desc->vn->filesize - desc->offset; // truncate size to the space we have left in the file.
    obos_status status = OBOS_STATUS_SUCCESS;
    if (desc->flags & FD_FLAGS_UNCACHED)
    {
        // Keep it nice and simple, and just do an uncached read on the file.

        status = do_uncached_read(desc, buf, nBytes, nRead);
    }
    else 
    {
        pagecache_dirty_region* dirty = VfsH_PCDirtyRegionLookup(&desc->vn->pagecache, desc->offset);
        if (dirty)
            Core_MutexAcquire(&dirty->lock);
        if ((desc->offset+nBytes) > desc->vn->pagecache.sz)
            VfsH_PageCacheResize(&desc->vn->pagecache, desc->vn, desc->offset + nBytes);
        memcpy(buf, desc->vn->pagecache.data + desc->offset, nBytes);
        if (dirty)
            Core_MutexRelease(&dirty->lock);

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
    size_t finalOff = 0;
    switch (whence)
    {
        case SEEK_SET:
            finalOff = off;
            break;
        case SEEK_END:
            finalOff = (off_t)desc->vn->filesize - 1 + off;
            break;
        case SEEK_CUR:
            finalOff = (off_t)desc->offset + off;
            break;
    }
    if (is_eof(desc->vn, finalOff))
        return OBOS_STATUS_EOF;
    const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &desc->vn->mount_point->fs_driver->driver->header : nullptr;
    if (desc->vn->vtype == VNODE_TYPE_CHR || desc->vn->vtype == VNODE_TYPE_BLK)
        driver = &desc->vn->un.device->driver->header;
    size_t blkSize = 0;
    driver->ftable.get_blk_size(desc->vn->desc, &blkSize);
    if (finalOff % blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    desc->offset = finalOff;
    return OBOS_STATUS_SUCCESS;
}
uoff_t Vfs_FdTellOff(const fd* desc)
{
    if (desc)
        return desc->offset;
    return (uoff_t)(-1);
}
size_t Vfs_FdGetBlkSz(const fd* desc)
{
    if (!desc)
        return (size_t)-1;
    const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &desc->vn->mount_point->fs_driver->driver->header : nullptr;
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
obos_status Vfs_FdIoctl(fd* desc, size_t nParameters, uint64_t request, ...)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    if (desc->vn->vtype != VNODE_TYPE_BLK && desc->vn->vtype != VNODE_TYPE_CHR)
        return OBOS_STATUS_INVALID_IOCTL;
    va_list list;
    va_start(list, request);
    obos_status status = desc->vn->un.device->driver->header.ftable.ioctl_var(nParameters, request, list);
    va_end(list);
    return status;
}
obos_status Vfs_FdFlush(fd* desc)
{
    if (!desc)
        return OBOS_STATUS_SUCCESS;
    if (desc->flags & FD_FLAGS_UNCACHED)
        return OBOS_STATUS_INVALID_OPERATION;
    VfsH_PageCacheFlush(&desc->vn->pagecache, desc->vn);
    return OBOS_STATUS_SUCCESS;
}
obos_status Vfs_FdClose(fd* desc)
{
    Vfs_FdFlush(desc);
    desc->flags &= ~FD_FLAGS_OPEN;
    vnode* vn = desc->vn;
    vn->refs--;
    return OBOS_STATUS_SUCCESS;
}