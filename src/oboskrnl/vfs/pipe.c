/*
 * oboskrnl/vfs/pipe.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <vfs/fd.h>
#include <vfs/pipe.h>
#include <vfs/dirent.h>
#include <vfs/vnode.h>
#include <vfs/alloc.h>

#include <locks/pushlock.h>
#include <locks/wait.h>
#include <locks/event.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <stdatomic.h>

#include <utils/string.h>

static obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    OBOS_UNUSED(blkOffset);
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // OBOS_ASSERT(!"untested");
    pipe_desc *pipe = (void*)desc;
    // while (!pipe->offset && pipe->vn->opened.nNodes > 1)
    //     OBOSS_SpinlockHint();
    Core_WaitOnObject(WAITABLE_OBJECT(pipe->evnt));
    if (pipe->vn->opened.nNodes == 1 && !pipe->offset)
        return OBOS_STATUS_EOF;
    if (blkCount > pipe->offset)
        blkCount = pipe->offset;
    Core_PushlockAcquire(&pipe->lock, true);
    memcpy(buf, (char*)pipe->buf+blkOffset, blkCount);
    pipe->offset -= blkCount;
    Core_PushlockRelease(&pipe->lock, true);
    if (nBlkRead)
        *nBlkRead = blkCount;
    return OBOS_STATUS_SUCCESS;
}
static obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    OBOS_UNUSED(blkOffset);
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // OBOS_ASSERT(!"untested");
    pipe_desc *pipe = (void*)desc;
    if (blkCount > pipe->pipe_size)
    {
        size_t written_count = 0;
        size_t curr_written_count = 0;
        off_t current_offset = 0;
        long bytes_left = blkCount;
        while (bytes_left > 0)
        {
            obos_status status = write_sync(desc, buf + current_offset, (size_t)bytes_left > pipe->pipe_size ? pipe->pipe_size : (size_t)bytes_left, 0, &curr_written_count);
            if (obos_is_error(status))
                return status;
            written_count += curr_written_count;
            bytes_left -= curr_written_count;
            current_offset += curr_written_count;
        }
        if (nBlkWritten)
            *nBlkWritten = written_count;
        return OBOS_STATUS_SUCCESS;
    }
    bool atomic = blkCount <= PIPE_BUF;
    OBOS_UNUSED(atomic);
    // Core_PushlockAcquire(&pipe->lock, !atomic /* if we want to do an unatomic write, then we can be a reader, otherwise, we must be a writer */);
    Core_PushlockAcquire(&pipe->lock, false);
    memcpy((char*)pipe->buf + pipe->offset, buf, blkCount);
    pipe->offset += blkCount;
    Core_PushlockRelease(&pipe->lock, false);
    OBOS_ENSURE(obos_is_success(Core_EventSet(&pipe->evnt, false)));
    // Core_PushlockRelease(&pipe->lock, !atomic);
    if (nBlkWritten)
        *nBlkWritten = blkCount;
    return OBOS_STATUS_SUCCESS;
}
static obos_status get_blk_size(dev_desc desc, size_t* blkSize)
{
    if (!blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    OBOS_UNUSED(desc);
    *blkSize = 1;
    return OBOS_STATUS_SUCCESS;
}
static obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    if (!desc || !count)
        return OBOS_STATUS_INVALID_ARGUMENT;
    pipe_desc *pipe = (void*)desc;
    *count = pipe->pipe_size;
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION static obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    OBOS_UNUSED(what);
    OBOS_UNUSED(request);
    OBOS_UNUSED(argp);
    return OBOS_STATUS_INVALID_IOCTL;
}
static obos_status remove_file(dev_desc desc)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    pipe_desc *pipe = (void*)desc;
    Vfs_Free(pipe->buf);
    Vfs_Free(pipe);
    return OBOS_STATUS_SUCCESS;
}

driver_id OBOS_FIFODriver = {
    .id=0,
    .header = {
        .magic=OBOS_DRIVER_MAGIC,
        .driverName="FIFO Driver",
        .ftable = {
            .read_sync = read_sync,
            .write_sync = write_sync,
            .ioctl = ioctl,
            .get_blk_size=get_blk_size,
            .get_max_blk_count=get_max_blk_count,
            .remove_file=remove_file,
        },
    }
};
vdev OBOS_FIFODriverVdev = {
    .driver = &OBOS_FIFODriver,
};

obos_status Vfs_CreatePipe(fd* fds, size_t pipesize)
{
    if (!fds)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!pipesize)
        pipesize = OBOS_PAGE_SIZE*16;
    if (pipesize < PIPE_BUF)
        pipesize = PIPE_BUF;
    pipe_desc *desc = Vfs_Calloc(1, sizeof(pipe_desc));
    desc->pipe_size = pipesize;
    desc->lock = PUSHLOCK_INITIALIZE();
    desc->buf = Allocate(OBOS_NonPagedPoolAllocator, desc->pipe_size, nullptr);
    desc->evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    vnode* vn = Vfs_Calloc(1, sizeof(vnode));
    desc->vn = vn;
    vn->desc = (uintptr_t)desc;
    memset(&vn->perm, 0xff, sizeof(vn->perm)); // lol
    vn->vtype = VNODE_TYPE_FIFO;
    vn->un.device = &OBOS_FIFODriverVdev;
    vn->filesize = pipesize;
    Vfs_FdOpenVnode(&fds[0], vn, FD_OFLAGS_READ);
    Vfs_FdOpenVnode(&fds[1], vn, FD_OFLAGS_WRITE);
    return OBOS_STATUS_SUCCESS;
}

obos_status Vfs_CreateNamedPipe(file_perm perm, gid group_uid, uid owner_uid, dirent *parent, const char* name, size_t pipesize)
{
    if (!parent || !name)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!pipesize)
        pipesize = OBOS_PAGE_SIZE*16;
    if (pipesize < PIPE_BUF)
        pipesize = PIPE_BUF;
    pipe_desc *desc = Vfs_Calloc(1, sizeof(pipe_desc));
    desc->pipe_size = pipesize;
    desc->buf = Allocate(OBOS_NonPagedPoolAllocator, desc->pipe_size, nullptr);
    desc->lock = PUSHLOCK_INITIALIZE();
    desc->evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    dirent* ent = Vfs_Calloc(1, sizeof(dirent));
    vnode* vn = Vfs_Calloc(1, sizeof(vnode));
    desc->vn = vn;
    vn->owner_uid = owner_uid;
    vn->group_uid = group_uid;
    vn->desc = (uintptr_t)desc;
    vn->perm = perm;
    vn->vtype = VNODE_TYPE_FIFO;
    vn->un.device = &OBOS_FIFODriverVdev;
    ent->vnode = vn;
    vn->refs++;
    vn->mount_point = parent->vnode->mount_point;
    OBOS_InitString(&ent->name, name);
    VfsH_DirentAppendChild(parent, ent);
    return OBOS_STATUS_SUCCESS;
}

void Vfs_InitializePipeInterface()
{
    // Amazing...
    // We don't need to do anything!
    return;
}
