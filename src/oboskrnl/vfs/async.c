/*
 * oboskrnl/vfs/alloc.c
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
#include <scheduler/thread_context_info.h>

#include <utils/list.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <locks/event.h>

#include <driver_interface/driverId.h>
#include <driver_interface/header.h>

// if depth == zero, it will search all nodes starting at 'from'
// otherwise, it will only search 'depth' nodes

static bool is_eof(vnode* vn, size_t off)
{
    return off >= vn->filesize;
}

struct async_irp
{
    // This event object is set the operation is finished.
    event* e;
    union {
        const void* cbuf;
        void* buf;
    } un;
    size_t requestSize;
    thread* worker;
    bool rw : 1; // if false, the operation is a read, otherwise it is a write.
    bool cached : 1;
    uoff_t fileoff;
    vnode* vn;
};

static void async_read(struct async_irp* irp)
{
    const driver_header* driver = irp->vn->vtype == VNODE_TYPE_REG ? &irp->vn->mount_point->fs_driver->driver->header : nullptr;
    if (irp->vn->vtype == VNODE_TYPE_CHR || irp->vn->vtype == VNODE_TYPE_BLK)
        driver = &irp->vn->un.device->driver->header;
    driver->ftable.read_sync(
        irp->vn->desc,
        irp->un.buf,
        irp->requestSize,
        irp->fileoff,
        nullptr
    );

    Core_EventSet(irp->e, true);
    Vfs_Free(irp);
    Core_ExitCurrentThread();
}
static void async_write(struct async_irp* irp)
{
    const driver_header* driver = irp->vn->vtype == VNODE_TYPE_REG ? &irp->vn->mount_point->fs_driver->driver->header : nullptr;
    if (irp->vn->vtype == VNODE_TYPE_CHR || irp->vn->vtype == VNODE_TYPE_BLK)
        driver = &irp->vn->un.device->driver->header;
    driver->ftable.write_sync(
        irp->vn->desc,
        irp->un.cbuf,
        irp->requestSize,
        irp->fileoff,
        nullptr
    );
    
    Core_EventSet(irp->e, true);
    Vfs_Free(irp);
    Core_GetCurrentThread();
}

obos_status Vfs_FdAWrite(fd* desc, const void* buf, size_t nBytes, event* evnt)
{
    if (!desc || !buf || !evnt)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    if (!nBytes)
        return OBOS_STATUS_SUCCESS;
    if (is_eof(desc->vn, desc->offset))
        return OBOS_STATUS_EOF;
    if (!(desc->flags & FD_FLAGS_WRITE))
        return OBOS_STATUS_ACCESS_DENIED;
    if (desc->flags & FD_FLAGS_UNCACHED)
    {
        // If an asynchronous, uncached read is made, then first check if nBytes < sector size.
        // If it is not, or the vnode is not backed by a drive, then an IRP is made.
        // Otherwise, the read is made, and the operation completes.
        size_t sector_size = 0;
        if (!desc->vn->mount_point->device)
            goto irp;
        desc->vn->mount_point->device->driver->header.ftable.get_blk_size(desc->vn->mount_point->device->desc, &sector_size);
        if (nBytes >= sector_size)
            goto irp;
        obos_status status = desc->vn->mount_point->fs_driver->driver->header.ftable.write_sync(
            desc->vn->desc,
            buf,
            nBytes,
            desc->offset,
            nullptr
        );
        Core_EventSet(evnt, true);
        desc->offset += nBytes;
        return status;
    }
    else 
    {
        if ((desc->offset) > desc->vn->pagecache.sz)
            goto irp;
        size_t nBytesToRead = nBytes;
        if ((nBytesToRead + desc->offset) > desc->vn->pagecache.sz)
            nBytesToRead -= ((nBytesToRead + desc->offset) - desc->vn->pagecache.sz);
        memcpy(desc->vn->pagecache.data + desc->offset, buf, nBytesToRead);
        if (!(nBytesToRead-nBytes))
        {
            desc->offset += nBytes;
            return OBOS_STATUS_SUCCESS;
        }
        buf += nBytesToRead;
        nBytes -= nBytesToRead;
    }
    irp:
    (void)0;
    struct async_irp* irp = Vfs_Calloc(1, sizeof(struct async_irp));
    irp->e = evnt;
    irp->rw = true;
    irp->fileoff = desc->offset;
    irp->cached = !(desc->flags & FD_FLAGS_UNCACHED);
    irp->requestSize = nBytes;
    irp->un.cbuf = buf;
    irp->vn = desc->vn;
    irp->worker = CoreH_ThreadAllocate(nullptr);
    thread_ctx ctx = {};
    CoreS_SetupThreadContext(
        &ctx, 
        (uintptr_t)async_write, (uintptr_t)irp,
        false,
        Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x10000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr), 
        0x10000);
    CoreH_ThreadInitialize(irp->worker, THREAD_PRIORITY_HIGH, Core_DefaultThreadAffinity, &ctx);
    Core_ProcessAppendThread(OBOS_KernelProcess, irp->worker);
    CoreH_ThreadReady(irp->worker);
    desc->offset += nBytes;
    return OBOS_STATUS_SUCCESS;
}
obos_status Vfs_FdARead(fd* desc, void* buf, size_t nBytes, event* evnt)
{
    if (!desc || !buf || !evnt)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    if (!nBytes)
        return OBOS_STATUS_SUCCESS;
    if (is_eof(desc->vn, desc->offset))
        return OBOS_STATUS_EOF;
    if (!(desc->flags & FD_FLAGS_READ))
        return OBOS_STATUS_ACCESS_DENIED;
    if (desc->flags & FD_FLAGS_UNCACHED)
    {
        // If an asynchronous, uncached read is made, then first check if nBytes < sector size.
        // If it is not, or the vnode is not backed by a drive, then an IRP is made.
        // Otherwise, the read is made, and the operation completes.
        size_t sector_size = 0;
        if (!desc->vn->mount_point->device)
            goto irp;
        desc->vn->mount_point->device->driver->header.ftable.get_blk_size(desc->vn->mount_point->device->desc, &sector_size);
        if (nBytes >= sector_size)
            goto irp;
        obos_status status = desc->vn->mount_point->fs_driver->driver->header.ftable.read_sync(
            desc->vn->desc,
            buf,
            nBytes,
            desc->offset,
            nullptr
        );
        Core_EventSet(evnt, true);
        desc->offset += nBytes;
        return status;
    }
    else 
    {
        if ((desc->offset) > desc->vn->pagecache.sz)
            goto irp;
        size_t nBytesToRead = nBytes;
        if ((nBytesToRead + desc->offset) > desc->vn->pagecache.sz)
            nBytesToRead -= ((nBytesToRead + desc->offset) - desc->vn->pagecache.sz);
        memcpy(buf, desc->vn->pagecache.data + desc->offset, nBytesToRead);
        if (!(nBytesToRead-nBytes))
        {
            desc->offset += nBytes;
            return OBOS_STATUS_SUCCESS;
        }
        buf += nBytesToRead;
        nBytes -= nBytesToRead;
    }
    irp:
    (void)0;
    struct async_irp* irp = Vfs_Calloc(1, sizeof(struct async_irp));
    irp->e = evnt;
    irp->rw = false;
    irp->fileoff = desc->offset;
    irp->cached = !(desc->flags & FD_FLAGS_UNCACHED);
    irp->requestSize = nBytes;
    irp->un.buf = buf;
    irp->vn = desc->vn;
    irp->worker = CoreH_ThreadAllocate(nullptr);
    thread_ctx ctx = {};
    CoreS_SetupThreadContext(
        &ctx, 
        (uintptr_t)async_read, (uintptr_t)irp,
        false,
        Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x10000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr), 
        0x10000);
    CoreH_ThreadInitialize(irp->worker, THREAD_PRIORITY_HIGH, Core_DefaultThreadAffinity, &ctx);
    irp->worker->stackFree = CoreH_VMAStackFree;
    irp->worker->stackFreeUserdata = &Mm_KernelContext;
    CoreH_ThreadReady(irp->worker);
    desc->offset += nBytes;
    return OBOS_STATUS_SUCCESS;
}