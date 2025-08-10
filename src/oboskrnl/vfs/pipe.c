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
#include <vfs/mount.h>
#include <vfs/vnode.h>
#include <vfs/alloc.h>
#include <vfs/irp.h>

#include <locks/pushlock.h>
#include <locks/wait.h>
#include <locks/event.h>

#include <scheduler/schedule.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <mm/alloc.h>

#include <stdatomic.h>

#include <utils/string.h>

#define IOCTL_PIPE_SET_SIZE 1
#define IOCTL_PIPE_GET_SIZE 2

static obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    OBOS_UNUSED(desc && buf && blkCount && blkOffset && nBlkRead);
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    pipe_desc* pipe = (void*)desc;
    //OBOS_Log("thread %d: enter %s. blkCount=%d, pipe->offset=%d, pipe->size=%d, pipe=%p\n", Core_GetCurrentThread()->tid, __func__, blkCount, pipe->offset, pipe->size, pipe);
    bool has_write_fd = false;
    for (fd* f = LIST_GET_HEAD(fd_list, &pipe->vn->opened); f; f = LIST_GET_NEXT(fd_list, &pipe->vn->opened, f))
    {
        if (f->flags & FD_FLAGS_WRITE)
        {
            has_write_fd = true;
            break;
        }
    }
    if (!has_write_fd && !pipe->read_event.hdr.signaled)
        return OBOS_STATUS_PIPE_CLOSED;
    obos_status status = OBOS_STATUS_SUCCESS;
    if (obos_is_error(status = Core_WaitOnObject(WAITABLE_OBJECT(pipe->read_event))))
        return status == OBOS_STATUS_ABORTED ? OBOS_STATUS_PIPE_CLOSED : status;
    Core_PushlockAcquire(&pipe->buffer_lock, true);
    memcpy(buf, pipe->buffer+pipe->offset, blkCount);
    pipe->offset = (pipe->offset + blkCount) % pipe->size;
    Core_EventSet(&pipe->write_event, false);
    if (!pipe->offset)
        Core_EventClear(&pipe->read_event);
    Core_PushlockRelease(&pipe->buffer_lock, true);
    //OBOS_Log("thread %d: ret from %s. blkCount=%d, pipe->offset=%d, pipe->size=%d, pipe=%p\n", Core_GetCurrentThread()->tid, __func__, blkCount, pipe->offset, pipe->size, pipe);
    if (nBlkRead)
        *nBlkRead = blkCount;
    return OBOS_STATUS_SUCCESS;
}
static obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    OBOS_UNUSED(desc && buf && blkCount && blkOffset && nBlkWritten);
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    pipe_desc* pipe = (void*)desc;
    //OBOS_Log("thread %d: enter %s. blkCount=%d, pipe->offset=%d, pipe->size=%d, pipe=%p\n", Core_GetCurrentThread()->tid, __func__, blkCount, pipe->offset, pipe->size, pipe);
    obos_status status = OBOS_STATUS_SUCCESS;
    if (obos_is_error(status = Core_WaitOnObject(WAITABLE_OBJECT(pipe->write_event))))
        return status;
    Core_PushlockAcquire(&pipe->buffer_lock, true);
    memcpy(pipe->buffer+pipe->offset, buf, blkCount);
    Core_EventSet(&pipe->read_event, false);
    Core_EventClear(&pipe->write_event);
    Core_PushlockRelease(&pipe->buffer_lock, true);
    //OBOS_Log("thread %d: ret from %s. blkCount=%d, pipe->offset=%d, pipe->size=%d, pipe=%p\n", Core_GetCurrentThread()->tid, __func__, blkCount, pipe->offset, pipe->size, pipe);
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
    *count = pipe->size;
    return OBOS_STATUS_SUCCESS;
}
static obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    if (!what)
        return OBOS_STATUS_INVALID_ARGUMENT;
    pipe_desc *pipe = (void*)what;
    size_t* sargp = argp;
    switch (request)
    {
        case IOCTL_PIPE_SET_SIZE:
        {
            if (*sargp == pipe->size)
                return OBOS_STATUS_SUCCESS;
            Core_PushlockAcquire(&pipe->buffer_lock, false);
            pipe->buffer = Vfs_Realloc(pipe->buffer, *sargp);
            pipe->size = *sargp;
            if (pipe->offset > pipe->size)
                pipe->offset = 0;
            pipe->vn->filesize = pipe->size;
            Core_PushlockRelease(&pipe->buffer_lock, false);
            break;
        }
        case IOCTL_PIPE_GET_SIZE:
        {
            Core_PushlockAcquire(&pipe->buffer_lock, true);
            *sargp = pipe->size;
            Core_PushlockRelease(&pipe->buffer_lock, true);
            break;
        }
        default: return OBOS_STATUS_INVALID_IOCTL;
    }
    return OBOS_STATUS_SUCCESS;
}
static obos_status ioctl_argp_size(uint32_t request, size_t *size)
{
    switch (request)
    {
        case IOCTL_PIPE_SET_SIZE:
        {
            *size = 8;
            break;
        }
        default: return OBOS_STATUS_INVALID_IOCTL;
    }
    return OBOS_STATUS_SUCCESS;
}
static obos_status reference_device(dev_desc* pdesc)
{
    if (!pdesc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    dev_desc desc = *pdesc;
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    pipe_desc* pipe = (void*)desc;
    //OBOS_Log("thread %d: enter %s. refs=%d, pipe->offset=%d, pipe->size=%d, pipe=%p\n", Core_GetCurrentThread()->tid, __func__, pipe->refs, pipe->offset, pipe->size, pipe);
    pipe->refs++;
    //OBOS_Log("thread %d: ret from %s. refs=%d, pipe->offset=%d, pipe->size=%d, pipe=%p\n", Core_GetCurrentThread()->tid, __func__, pipe->refs, pipe->offset, pipe->size, pipe);
    return OBOS_STATUS_SUCCESS;
}
static obos_status unreference_device(dev_desc desc)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    pipe_desc* pipe = (void*)desc;
    //OBOS_Log("thread %d: enter %s. refs=%d, pipe->offset=%d, pipe->size=%d, pipe=%p\n", Core_GetCurrentThread()->tid, __func__, pipe->refs, pipe->offset, pipe->size, pipe);
    if (pipe->refs == 2 && !pipe->read_event.hdr.signaled)
        CoreH_AbortWaitingThreads(WAITABLE_OBJECT(pipe->read_event));
    //OBOS_Log("thread %d: ret from %s. refs=%d, pipe->offset=%d, pipe->size=%d, pipe=%p\n", Core_GetCurrentThread()->tid, __func__, pipe->refs-1, pipe->offset, pipe->size, pipe);
    if (!(--pipe->refs))
    {
        Vfs_Free(pipe->buffer);
        Vfs_Free(pipe);
    }
    return OBOS_STATUS_SUCCESS;
}
static obos_status remove_file(dev_desc desc)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    return unreference_device(desc);   
}
static obos_status submit_irp(void* irp_)
{
    irp* req = irp_;
    if (!req || !req->desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    pipe_desc* pipe = (void*)req->desc;
    req->evnt = req->op == IRP_READ ? &pipe->read_event : &pipe->write_event;
    req->on_event_set = nullptr;
    req->status = OBOS_STATUS_SUCCESS;
    return OBOS_STATUS_SUCCESS;
}
static obos_status finalize_irp(void* irp_)
{
    irp* req = irp_;
    if (!req || !req->desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (req->dryOp)
        return OBOS_STATUS_SUCCESS;
    req->status = req->op == IRP_READ ? 
        read_sync(req->desc, req->buff, req->blkCount, req->blkOffset, &req->nBlkRead) :
        write_sync(req->desc, req->cbuff, req->blkCount, req->blkOffset, &req->nBlkWritten);
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
            .ioctl_argp_size = ioctl_argp_size,
            .get_blk_size=get_blk_size,
            .get_max_blk_count=get_max_blk_count,
            .remove_file=remove_file,
            .reference_device = reference_device,
            .unreference_device = unreference_device,
            .submit_irp = submit_irp,
            .finalize_irp = finalize_irp,
        },
    }
};
vdev OBOS_FIFODriverVdev = {
    .driver = &OBOS_FIFODriver,
};

pipe_desc* alloc_pipe_desc(size_t pipesize)
{
    pipe_desc* desc = Vfs_Calloc(1, sizeof(pipe_desc));
    desc->size = pipesize;
    desc->buffer = Vfs_Malloc(pipesize);
    desc->read_event = EVENT_INITIALIZE(EVENT_SYNC);
    desc->write_event = EVENT_INITIALIZE(EVENT_SYNC);
    desc->buffer_lock = PUSHLOCK_INITIALIZE();
    Core_EventSet(&desc->write_event, false);
    return desc;
}

obos_status Vfs_CreatePipe(fd* fds, size_t pipesize)
{
    if (!fds)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!pipesize)
        pipesize = OBOS_PAGE_SIZE;
    if (pipesize < PIPE_BUF)
        pipesize = PIPE_BUF;
    pipe_desc *desc = alloc_pipe_desc(pipesize);
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
    if (!name)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!parent) parent = Vfs_Root;
    if (VfsH_DirentLookupFrom(name, parent))
        return OBOS_STATUS_ALREADY_INITIALIZED;
    if (!pipesize)
        pipesize = OBOS_PAGE_SIZE;
    if (pipesize < PIPE_BUF)
        pipesize = PIPE_BUF;
    pipe_desc *desc = alloc_pipe_desc(pipesize);
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
    vn->filesize = desc->size;
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
