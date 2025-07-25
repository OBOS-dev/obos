/*
 * oboskrnl/vfs/fd.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
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
#include <vfs/irp.h>
#include <vfs/pipe.h>

#include <allocators/base.h>

#include <locks/event.h>
#include <locks/wait.h>

#include <mm/swap.h>

#include <scheduler/schedule.h>
#include <scheduler/process.h>
#include <scheduler/thread.h>

#include <utils/list.h>

#include <locks/mutex.h>

#include <irq/irql.h>

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
    ent = VfsH_FollowLink(ent);
    if (!ent)
        return OBOS_STATUS_NOT_FOUND;
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
    if (vnode->vtype == VNODE_TYPE_DIR || vnode->vtype == VNODE_TYPE_LNK)
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
            desc->flags &= ~FD_FLAGS_READ;
        if (!vn->perm.owner_write)
            desc->flags &= ~FD_FLAGS_WRITE;
        if (oflags & FD_OFLAGS_EXECUTE && !vn->perm.owner_exec)
            return OBOS_STATUS_ACCESS_DENIED;
    }
    else if (desc->vn->group_uid == Core_GetCurrentThread()->proc->currentGID)
    {
        // We have group perms.
        struct vnode* const vn = desc->vn;
        if (!vn->perm.group_read)
            desc->flags &= ~FD_FLAGS_READ;
        if (!vn->perm.group_write)
            desc->flags &= ~FD_FLAGS_WRITE;
        if (oflags & FD_OFLAGS_EXECUTE && !vn->perm.group_exec)
            return OBOS_STATUS_ACCESS_DENIED;
    }
    else
    {
        // We have other perms.
        struct vnode* const vn = desc->vn;
        if (!vn->perm.other_read)
            desc->flags &= ~FD_FLAGS_READ;
        if (!vn->perm.other_write)
            desc->flags &= ~FD_FLAGS_WRITE;
        if (oflags & FD_OFLAGS_EXECUTE && !vn->perm.other_exec)
            return OBOS_STATUS_ACCESS_DENIED;
    }
    if (~oflags & FD_OFLAGS_READ)
        desc->flags &= ~FD_FLAGS_READ;
    if (~oflags & FD_OFLAGS_WRITE)
        desc->flags &= ~FD_FLAGS_WRITE;
    if (oflags & FD_OFLAGS_UNCACHED)
        desc->flags |= FD_FLAGS_UNCACHED;
    if (oflags & FD_OFLAGS_NOEXEC)
        desc->flags |= FD_FLAGS_NOEXEC;
    if (vnode->vtype == VNODE_TYPE_CHR || vnode->vtype == VNODE_TYPE_FIFO)
        desc->flags |= FD_FLAGS_UNCACHED;
    if (vnode->flags & VFLAGS_EVENT_DEV)
        desc->flags &= ~(FD_FLAGS_READ|FD_FLAGS_WRITE);
    desc->vn->refs++;
    LIST_APPEND(fd_list, &desc->vn->opened, desc);
    if (~desc->vn->flags & VFLAGS_EVENT_DEV)
    {
        mount* point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
        const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
        if (desc->vn->vtype == VNODE_TYPE_CHR || desc->vn->vtype == VNODE_TYPE_BLK || desc->vn->vtype == VNODE_TYPE_FIFO)
        {
            point = nullptr;
            driver = &desc->vn->un.device->driver->header;
        }
        desc->desc = desc->vn->desc;
        if (driver->ftable.reference_device)
            driver->ftable.reference_device(&desc->desc);
    }
    desc->flags |= FD_FLAGS_OPEN;
    return OBOS_STATUS_SUCCESS;
}
static obos_status do_uncached_write(fd* desc, const void* from, size_t nBytes, size_t* nWritten_, size_t uoffset)
{
    // First, try making an IRP.
    irp* req = VfsH_IRPAllocate();
    VfsH_IRPBytesToBlockCount(desc->vn, nBytes, &req->blkCount);
    VfsH_IRPBytesToBlockCount(desc->vn, uoffset, &req->blkOffset);
    req->cbuff = from;
    VfsH_IRPRef(req);
    req->op = IRP_WRITE;
    req->dryOp = false;
    req->status = OBOS_STATUS_SUCCESS;
    req->vn = desc->vn;
    obos_status status = VfsH_IRPSubmit(req, &desc->desc);
    if (obos_is_success(status))
    {
        status = VfsH_IRPWait(req);
        VfsH_IRPUnref(req);
        if (nWritten_)
            *nWritten_ = req->nBlkRead * desc->vn->blkSize;
        return status;
    }
    VfsH_IRPUnref(req);

    // Unimplemented, so fallback to write_sync

    mount* point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
    const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (desc->vn->vtype == VNODE_TYPE_CHR || desc->vn->vtype == VNODE_TYPE_BLK || desc->vn->vtype == VNODE_TYPE_FIFO)
    {
        point = nullptr;
        driver = &desc->vn->un.device->driver->header;
    }
    size_t blkSize = 0;
    driver->ftable.get_blk_size(desc->desc, &blkSize);
    if (nBytes % blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    nBytes /= blkSize;
    const size_t base_offset = desc->vn->flags & VFLAGS_PARTITION ? desc->vn->partitions[0].off : 0;
    const uintptr_t offset = (uoffset + base_offset) / blkSize;
    if (!VfsH_LockMountpoint(point))
        return OBOS_STATUS_ABORTED;
    status = driver->ftable.write_sync(desc->desc, from, nBytes, offset, nWritten_);
    VfsH_UnlockMountpoint(point);
    if (obos_expect(obos_is_error(status) == true, 0))
        return status;
    if (nWritten_)
        *nWritten_ *= blkSize;
    return OBOS_STATUS_SUCCESS;
}
obos_status Vfs_FdWrite(fd* desc, const void* buf, size_t nBytes, size_t* nWritten)
{
    obos_status status = Vfs_FdPWrite(desc, buf, desc ? desc->offset : SIZE_MAX, nBytes, nWritten);
    if (obos_expect(obos_is_success(status), 1))
    {
        if (nBytes > (desc->vn->filesize - desc->offset) && desc->vn->vtype == VNODE_TYPE_REG)
            desc->vn->filesize += (nBytes-(desc->vn->filesize - desc->offset)); // add the difference to the file size
        Vfs_FdSeek(desc, nBytes, SEEK_CUR);
    }
    return status;
}
static obos_status do_uncached_read(fd* desc, void* into, size_t nBytes, size_t* nRead_, size_t uoffset)
{
    // First, try making an IRP.
    irp* req = VfsH_IRPAllocate();
    VfsH_IRPBytesToBlockCount(desc->vn, nBytes, &req->blkCount);
    VfsH_IRPBytesToBlockCount(desc->vn, uoffset, &req->blkOffset);
    req->buff = into;
    req->op = IRP_READ;
    req->dryOp = false;
    req->status = OBOS_STATUS_SUCCESS;
    req->vn = desc->vn;
    obos_status status = VfsH_IRPSubmit(req, &desc->desc);
    if (obos_is_success(status))
    {
        obos_status status = VfsH_IRPWait(req);
        if (nRead_)
            *nRead_ = req->nBlkRead * desc->vn->blkSize;
        VfsH_IRPUnref(req);
        return status;
    }
    VfsH_IRPUnref(req);

    // Unimplemented, so fallback to read_sync

    mount* point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
    const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (desc->vn->vtype == VNODE_TYPE_CHR || desc->vn->vtype == VNODE_TYPE_BLK || desc->vn->vtype == VNODE_TYPE_FIFO)
    {
        point = nullptr;
        driver = &desc->vn->un.device->driver->header;
    }
    size_t blkSize = 0;
    driver->ftable.get_blk_size(desc->desc, &blkSize);
    if (nBytes % blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    nBytes /= blkSize;
    const size_t base_offset = desc->vn->flags & VFLAGS_PARTITION ? desc->vn->partitions[0].off : 0;
    const uintptr_t offset = (uoffset+base_offset) / blkSize;
    if (desc->vn->vtype == VNODE_TYPE_REG && !VfsH_LockMountpoint(point))
        return OBOS_STATUS_ABORTED;
    status = driver->ftable.read_sync(desc->desc, into, nBytes, offset, nRead_);
    if (desc->vn->vtype == VNODE_TYPE_REG)
        VfsH_UnlockMountpoint(point);
    if (obos_expect(obos_is_error(status) == true, 0))
        return status;
    if (nRead_)
        *nRead_ *= blkSize;
    return OBOS_STATUS_SUCCESS;
}
obos_status Vfs_FdRead(fd* desc, void* buf, size_t nBytes, size_t* nRead)
{
    if (!desc || !desc->vn)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (nBytes > (desc->vn->filesize - (desc ? desc->offset : SIZE_MAX)) && desc->vn->vtype != VNODE_TYPE_CHR)
        nBytes = desc->vn->filesize - (desc ? desc->offset : SIZE_MAX);
    obos_status status = Vfs_FdPRead(desc, buf, desc ? desc->offset : SIZE_MAX, nBytes, nRead);
    if (obos_expect(obos_is_success(status), 1))
        Vfs_FdSeek(desc, nBytes, SEEK_CUR);
    return status;
}
obos_status Vfs_FdPWrite(fd* desc, const void* buf, size_t offset, size_t nBytes, size_t* nWritten)
{
    if (!desc || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    if (!nBytes)
        return OBOS_STATUS_SUCCESS;
    if (is_eof(desc->vn, desc->offset))
        return OBOS_STATUS_EOF;
    if (nBytes > (desc->vn->filesize - offset) && desc->vn->vtype == VNODE_TYPE_BLK)
        return OBOS_STATUS_EOF;
    if (!(desc->flags & FD_FLAGS_WRITE))
        return OBOS_STATUS_ACCESS_DENIED;
    obos_status status = OBOS_STATUS_SUCCESS;
    if (desc->flags & FD_FLAGS_UNCACHED)
    {
        // Keep it nice and simple, and just do an uncached write on the file.

        status = do_uncached_write(desc, buf, nBytes, nWritten, offset);
    }
    else 
    {
        mount* point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
        if (!VfsH_LockMountpoint(point))
            return OBOS_STATUS_ABORTED;

        size_t start = offset;
        size_t end = offset + nBytes;
        page* pg = nullptr;
        if ((start & ~(OBOS_PAGE_SIZE-1)) == (end & ~(OBOS_PAGE_SIZE-1)))
        {
            // The start and end are on the same page, and therefore, 
            // use the same pagecache entry.
        
            void* ent = VfsH_PageCacheGetEntry(desc->vn, start, &pg);
            if (!ent)
                return OBOS_STATUS_INVALID_OPERATION;

            memcpy(ent, buf, nBytes);
            Mm_MarkAsDirtyPhys(pg);
        }
        else
        {
            size_t i = 0;
            size_t end_rounded = end;
            if (end_rounded % OBOS_PAGE_SIZE)
                end_rounded = (end_rounded + (OBOS_PAGE_SIZE-(end_rounded%OBOS_PAGE_SIZE)));
            for (size_t curr = start; curr < end_rounded && i < nBytes; )
            {
                uint8_t* ent = VfsH_PageCacheGetEntry(desc->vn, curr, &pg);
                if (!ent)
                    return OBOS_STATUS_INVALID_OPERATION;
                size_t nToRead = OBOS_PAGE_SIZE-((uintptr_t)ent % OBOS_PAGE_SIZE);
                nToRead = OBOS_MIN(nToRead, nBytes-i);
                memcpy(ent, (const void*)((uintptr_t)buf+i), nToRead);
                curr += nToRead;
                i += nToRead;
                Mm_MarkAsDirtyPhys(pg);
            }
        }
        VfsH_UnlockMountpoint(point);

        if (nWritten)
            *nWritten = nBytes;
    }
    return status;
}
obos_status Vfs_FdPRead(fd* desc, void* buf, size_t offset, size_t nBytes, size_t* nRead)
{
    if (!desc || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    if (!nBytes)
        return OBOS_STATUS_SUCCESS;
    if (is_eof(desc->vn, offset))
        return OBOS_STATUS_EOF;
    if (!(desc->flags & FD_FLAGS_READ))
        return OBOS_STATUS_ACCESS_DENIED;
    if (nBytes > (desc->vn->filesize - offset) && desc->vn->vtype != VNODE_TYPE_CHR)
        nBytes = desc->vn->filesize - offset; // truncate size to the space we have left in the file.
    obos_status status = OBOS_STATUS_SUCCESS;
    if (desc->flags & FD_FLAGS_UNCACHED)
    {
        // Keep it nice and simple, and just do an uncached read on the file.
    
        status = do_uncached_read(desc, buf, nBytes, nRead, offset);
    }
    else 
    {
        mount* point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
        // const size_t base_offset = desc->vn->flags & VFLAGS_PARTITION ? desc->vn->partitions[0].off : 0;
        if (!VfsH_LockMountpoint(point))
            return OBOS_STATUS_ABORTED;
    
        size_t start = offset;
        size_t end = offset + nBytes;
        if ((start & ~(OBOS_PAGE_SIZE-1)) == (end & ~(OBOS_PAGE_SIZE-1)))
        {
            // The start and end are on the same page, and therefore, 
            // use the same pagecache entry.
        
            void* ent = VfsH_PageCacheGetEntry(desc->vn, start, nullptr);
            if (!ent)
                return OBOS_STATUS_INVALID_OPERATION;

            memcpy(buf, ent, nBytes);
        }
        else
        {
            size_t i = 0;
            size_t end_rounded = end;
            if (end_rounded % OBOS_PAGE_SIZE)
                end_rounded = (end_rounded + (OBOS_PAGE_SIZE-(end_rounded%OBOS_PAGE_SIZE)));
            for (size_t curr = start; curr < end_rounded && i < nBytes; )
            {
                uint8_t* ent = VfsH_PageCacheGetEntry(desc->vn, curr, nullptr);

                if (!ent)
                    return OBOS_STATUS_INVALID_OPERATION;
                size_t nToRead = OBOS_PAGE_SIZE-((uintptr_t)ent % OBOS_PAGE_SIZE);
                nToRead = OBOS_MIN(nToRead, nBytes-i);
                memcpy((void*)((uintptr_t)buf+i), ent, nToRead);
                curr += nToRead;
                i += nToRead;
            }
        }
    
        VfsH_UnlockMountpoint(point);
    
        if (nRead)
            *nRead = nBytes;
    }
    return status;
}
obos_status Vfs_FdSeek(fd* desc, off_t off, whence_t whence)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    // if (desc->vn->vtype == VNODE_TYPE_FIFO)
    //     return OBOS_STATUS_SUCCESS; // act like it worked
    size_t finalOff = 0;
    mount* point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
    const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (desc->vn->vtype == VNODE_TYPE_CHR || desc->vn->vtype == VNODE_TYPE_BLK || desc->vn->vtype == VNODE_TYPE_FIFO)
    {
        point = nullptr;
        driver = &desc->vn->un.device->driver->header;
    }
    size_t blkSize = 0;
    driver->ftable.get_blk_size(desc->desc, &blkSize);
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
    if (is_eof(desc->vn, finalOff) && desc->vn->vtype != VNODE_TYPE_FIFO)
        return OBOS_STATUS_EOF;
    desc->offset = finalOff;
    return OBOS_STATUS_SUCCESS;
}
uoff_t Vfs_FdTellOff(const fd* desc)
{
    if (desc && desc->vn)
        return desc->offset;
    return (uoff_t)(-1);
}
size_t Vfs_FdGetBlkSz(const fd* desc)
{
    if (!desc)
        return (size_t)-1;
    mount* point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
    const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (desc->vn->vtype == VNODE_TYPE_CHR || desc->vn->vtype == VNODE_TYPE_BLK || desc->vn->vtype == VNODE_TYPE_FIFO)
    {
        point = nullptr;
        driver = &desc->vn->un.device->driver->header;
    }
    size_t blkSize = 0;
    driver->ftable.get_blk_size(desc->desc, &blkSize);
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
    return desc->vn->un.device->driver->header.ftable.ioctl(desc->desc, request, argp);
}
obos_status Vfs_FdFlush(fd* desc)
{
    if (!desc)
        return OBOS_STATUS_SUCCESS;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (desc->flags & FD_FLAGS_UNCACHED)
        return OBOS_STATUS_INVALID_OPERATION;
    mount* point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
    if (!VfsH_LockMountpoint(point))
        return OBOS_STATUS_ABORTED;
    Mm_PageWriterOperation = PAGE_WRITER_SYNC_FILE;
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
    mount* point = desc->vn->mount_point ? desc->vn->mount_point : desc->vn->un.mounted;
    const driver_header* driver = desc->vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (desc->vn->vtype == VNODE_TYPE_CHR || desc->vn->vtype == VNODE_TYPE_BLK || desc->vn->vtype == VNODE_TYPE_FIFO)
    {
        point = nullptr;
        driver = &desc->vn->un.device->driver->header;
    }
    if (!VfsH_LockMountpoint(point))
        return OBOS_STATUS_ABORTED;
    if (driver->ftable.unreference_device && driver->ftable.reference_device)
        driver->ftable.unreference_device(desc->desc);
    vnode* vn = desc->vn;
    LIST_REMOVE(fd_list, &desc->vn->opened, desc);
    if (vn->vtype == VNODE_TYPE_FIFO)
    {
        pipe_desc* desc = (void*)vn->desc;
        Core_EventSet(&desc->evnt, true);
    }
    vn->refs--;
    desc->flags &= ~FD_FLAGS_OPEN;
    
    VfsH_UnlockMountpoint(point);
    return OBOS_STATUS_SUCCESS;
}
LIST_GENERATE_INTERNAL(fd_list, struct fd, node, OBOS_EXPORT);

obos_status VfsH_IRPBytesToBlockCount(vnode* vn, size_t nBytes, size_t *out)
{
    if (!vn || !out)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!nBytes)
    {
        *out = 0;
        return OBOS_STATUS_SUCCESS;
    }
    mount* point = vn->mount_point ? vn->mount_point : vn->un.mounted;
    const driver_header* driver = vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK || vn->vtype == VNODE_TYPE_FIFO)
        driver = &vn->un.device->driver->header;
    if (!vn->blkSize)
        driver->ftable.get_blk_size(vn->desc, &vn->blkSize);

    *out = nBytes / vn->blkSize;
    return OBOS_STATUS_SUCCESS;
}
obos_status VfsH_IRPSubmit(irp* request, const dev_desc* desc)
{
    vnode* const vn = request->vn;
    if (vn->flags & VFLAGS_EVENT_DEV)
    {
        request->evnt = vn->un.evnt;
        return OBOS_STATUS_SUCCESS;
    }
    if (!request || !vn)
        return OBOS_STATUS_INVALID_ARGUMENT;
    mount* point = vn->mount_point ? vn->mount_point : vn->un.mounted;
    const driver_header* driver = vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK || vn->vtype == VNODE_TYPE_FIFO)
        driver = &vn->un.device->driver->header;
    if (!vn->blkSize)
        driver->ftable.get_blk_size(vn->desc, &vn->blkSize);

    const size_t base_offset = vn->flags & VFLAGS_PARTITION ? (vn->partitions[0].off / vn->blkSize) : 0;
    const uintptr_t offset = request->blkOffset + base_offset;
    request->blkOffset = offset;

    if (!desc)
        request->desc = vn->desc;
    else
        request->desc = *desc;

    if (!driver->ftable.submit_irp)
        return OBOS_STATUS_UNIMPLEMENTED;

    return driver->ftable.submit_irp(request);
}

obos_status VfsH_IRPWait(irp* request)
{
    OBOS_ENSURE(Core_GetIrql() <= IRQL_DISPATCH);
    if (!request || !request->vn)
        return OBOS_STATUS_INVALID_ARGUMENT;
    vnode* const vn = request->vn;
    while (request->evnt)
    {
        Core_WaitOnObject(WAITABLE_OBJECT(*request->evnt));
        if (request->on_event_set)
            request->on_event_set(request);
        if (request->status != OBOS_STATUS_IRP_RETRY)
            break;
    }
    if (vn->flags & VFLAGS_EVENT_DEV)
        return OBOS_STATUS_SUCCESS;
    // If request-evnt == nullptr, there is data available immediately.
    mount* point = vn->mount_point ? vn->mount_point : vn->un.mounted;
    const driver_header* driver = vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK || vn->vtype == VNODE_TYPE_FIFO)
        driver = &vn->un.device->driver->header;
    if (driver->ftable.finalize_irp)
        driver->ftable.finalize_irp(request);
    return request->status;
}

obos_status VfsH_IRPSignal(irp* request, obos_status status)
{
    request->status = status;
    return Core_EventSet(request->evnt, true);
}

void VfsH_IRPRef(irp* request)
{
    if (request)
        ++(request->refs);
}
void VfsH_IRPUnref(irp* request)
{
    if (request && !(--request->refs))
        Free(OBOS_NonPagedPoolAllocator, request, sizeof(*request));
}
irp* VfsH_IRPAllocate()
{
    irp* ret = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(irp), nullptr);
    ret->refs = 1;
    return ret;
}
