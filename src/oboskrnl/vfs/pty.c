/*
 * oboskrnl/vfs/pty.c
 *
 * Copyright (c) 2026 Omar Berrow
 *
 * Psuedo-terminals
*/

#include <int.h>
#include <error.h>
#include <limits.h>
#include <klog.h>
#include <memmanip.h>
#include <signal.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <vfs/tty.h>
#include <vfs/irp.h>
#include <vfs/alloc.h>
#include <vfs/create.h>
#include <vfs/mount.h>

#include <locks/mutex.h>
#include <locks/event.h>

#include <utils/shared_ptr.h>

typedef struct pty {
    shared_ptr ptr;

    size_t master_refs;

    void(*data_ready)(void* tty, const void* buf, size_t nBytesReady);
    void* tty;

    dirent* slave;
    int slave_idx;

    struct {
        char buffer[4096];
        intptr_t ptr;
        intptr_t in_ptr;
        event data_evnt;
        event empty_evnt;
        event write_evnt;
        mutex lock;
    } output_buffer;
} pty;

static obos_status pty_output_write(pty* stream, const void* buffer, size_t sz, size_t *bytes_written)
{
    if (!stream || !buffer)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if ((sz+stream->output_buffer.ptr) >= sizeof(stream->output_buffer.buffer))
        sz = sizeof(stream->output_buffer.buffer) - stream->output_buffer.ptr;
    Core_MutexAcquire(&stream->output_buffer.lock);
    void* out_ptr = &stream->output_buffer.buffer[stream->output_buffer.ptr];    
    memcpy(out_ptr, buffer, sz);
    stream->output_buffer.ptr += sz;
    Core_EventSet(&stream->output_buffer.data_evnt, false);
    Core_EventClear(&stream->output_buffer.empty_evnt);
    Core_MutexRelease(&stream->output_buffer.lock);
    if (bytes_written)
        *bytes_written = sz;
    return OBOS_STATUS_SUCCESS;
}

static obos_status pty_output_ready_count(pty* stream, size_t* bytes_ready)
{
    if (!bytes_ready || !stream)
        return OBOS_STATUS_INVALID_ARGUMENT;

    Core_MutexAcquire(&stream->output_buffer.lock);
    *bytes_ready = stream->output_buffer.ptr - stream->output_buffer.in_ptr;
    Core_MutexRelease(&stream->output_buffer.lock);
    return OBOS_STATUS_SUCCESS;
}

static obos_status pty_output_read(pty* stream, void* buffer, size_t sz, size_t* bytes_read, bool peek)
{
    if (!stream || !buffer)
        return OBOS_STATUS_INVALID_ARGUMENT;
    OBOS_ASSERT(stream->output_buffer.ptr >= stream->output_buffer.in_ptr);
    if ((intptr_t)sz > (stream->output_buffer.ptr - stream->output_buffer.in_ptr))
        sz = (stream->output_buffer.ptr - stream->output_buffer.in_ptr);
    Core_MutexAcquire(&stream->output_buffer.lock);
    const void* in_ptr = &stream->output_buffer.buffer[stream->output_buffer.in_ptr];
    memcpy(buffer, in_ptr, sz);
    if (!peek)
    {
        stream->output_buffer.in_ptr += sz;
        if (stream->output_buffer.ptr == stream->output_buffer.in_ptr)
        {
            // Clear the data event, and set the pipe empty event.
            stream->output_buffer.in_ptr = 0;
            stream->output_buffer.ptr = 0;
            Core_EventSet(&stream->output_buffer.empty_evnt, false);
            Core_EventClear(&stream->output_buffer.data_evnt);
        }
        Core_EventSet(&stream->output_buffer.write_evnt, false);
    }
    Core_MutexRelease(&stream->output_buffer.lock);
    if (bytes_read)
        *bytes_read = sz;
    return OBOS_STATUS_SUCCESS;
}

static obos_status ptmx_get_blk_size(dev_desc desc, size_t* blkSize);
static obos_status ptmx_get_max_blk_count(dev_desc desc, size_t* count);
static obos_status ptmx_read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead);
static obos_status ptmx_write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);
static obos_status ptmx_submit_irp(void* /* irp* */ request);
static obos_status ptmx_finalize_irp(void* /* irp* */ request);
static obos_status ptmx_reference_device(dev_desc* desc);
static obos_status ptmx_unreference_device(dev_desc);
static obos_status ptmx_ioctl(dev_desc what, uint32_t request, void* argp);
static obos_status ptmx_ioctl_argp_size(uint32_t request, size_t* ret);

driver_id OBOS_PTMXDriver = {
    .id = 0,
    .header = {.magic = OBOS_DRIVER_MAGIC,
               .flags = DRIVER_HEADER_FLAGS_NO_ENTRY |
                        DRIVER_HEADER_HAS_VERSION_FIELD |
                        DRIVER_HEADER_HAS_STANDARD_INTERFACES,
               .ftable =
                   {
                       .get_blk_size = ptmx_get_blk_size,
                       .get_max_blk_count = ptmx_get_max_blk_count,
                       .write_sync = ptmx_write_sync,
                       .read_sync = ptmx_read_sync,
                       .ioctl = ptmx_ioctl,
                       .ioctl_argp_size = ptmx_ioctl_argp_size,
                       .submit_irp = ptmx_submit_irp,
                       .finalize_irp = ptmx_finalize_irp,
                       .reference_device = ptmx_reference_device,
                       .unreference_device = ptmx_unreference_device,
                       .driver_cleanup_callback = nullptr,
                   },
               .driverName = "'/dev/ptmx'"}};

void pty_set_data_ready_cb(void* tty_, void(*cb)(void* tty, const void* buf, size_t nBytesReady))
{
    tty* tty = tty_;
    OBOS_ASSERT(tty);

    struct pty* master = tty->interface.userdata;
    master->tty = tty;
    master->data_ready = cb;
}

// uhHHHhhh what do you mean i took this from pipe.c??
// i would never!
obos_status pty_write(void* tty_, const char* buf, size_t szBuf)
{
    tty* tty = tty_;
    OBOS_ASSERT(tty);

    struct pty* master = tty->interface.userdata;
    OBOS_ASSERT(master);

    if (sizeof(master->output_buffer.buffer) < szBuf)
    {
        size_t written_count = 0, tmp = 0;
        const char* write_buf = buf;
        obos_status status = OBOS_STATUS_SUCCESS;
        while ((written_count += tmp) < szBuf && obos_is_success(status))
        {
            size_t nToWrite = (szBuf - written_count) % sizeof(master->output_buffer.buffer);
            if (!nToWrite)
                nToWrite = sizeof(master->output_buffer.buffer);
            status = pty_write(tty_, write_buf + written_count, nToWrite);
            tmp = nToWrite;
        }
        return status;
    }

    while (true)
    {
        Core_EventClear(&master->output_buffer.write_evnt);
        size_t ready_count = 0;
        pty_output_ready_count(master, &ready_count);
        if ((sizeof(master->output_buffer.buffer) - ready_count) >= szBuf)
            break;
        obos_status status = Core_WaitOnObject(WAITABLE_OBJECT(master->output_buffer.write_evnt));
        if (obos_is_error(status))
            return status;
    }

    return pty_output_write(master, buf, szBuf, nullptr);
}

obos_status tcdrain(void* tty);

void pty_ref(void* tty) { OBOS_SharedPtrRef(&(((struct pty*)(((struct tty*)tty)->interface.userdata))->ptr)); }
void pty_deref(void* tty) { OBOS_SharedPtrUnref(&(((struct pty*)(((struct tty*)tty)->interface.userdata))->ptr)); }

tty_interface Vfs_PTSInterface = {
    .set_data_ready_cb = pty_set_data_ready_cb,
    .write = pty_write,
    .ref = pty_ref,
    .deref = pty_deref,
};

obos_status Vfs_CreatePTMX()
{
    vnode *vn = Drv_AllocateVNode(&OBOS_PTMXDriver, (dev_desc)1, 0, nullptr,
                                VNODE_TYPE_CHR);
    vn->flags |= VFLAGS_PTMX;
    vn->gid = 5; // tty
    OBOS_Log("%s: Creating /dev/ptmx\n", __func__);
    Drv_RegisterVNode(vn, "ptmx");
    OBOS_Log("%s: Creating /dev/pts\n", __func__);
    obos_status status = Vfs_CreateNode(Vfs_DevRoot, "pts", VNODE_TYPE_DIR, (file_perm){.mode=0755});
    if (status != OBOS_STATUS_ALREADY_INITIALIZED)
        OBOS_ENSURE(obos_is_success(status));
    return OBOS_STATUS_SUCCESS;
}

static void free_pty(void* udata, struct shared_ptr* ptr)
{
    OBOS_UNUSED(udata);
    struct pty* pty = ptr->obj;
    if (pty->slave)
        Vfs_FreeTTY(pty->slave->vnode->tty);
    Vfs_Free(pty);
}

obos_status VfsH_MakePTM(dev_desc* ptm_out)
{
    if (!ptm_out)
        return OBOS_STATUS_INVALID_ARGUMENT;

    struct pty* master = Vfs_Calloc(1, sizeof(*master));
    OBOS_SharedPtrConstruct(&master->ptr, master);
    master->ptr.free = free_pty;
    master->ptr.freeUdata = nullptr;
    OBOS_SharedPtrRef(&master->ptr);
    
    master->output_buffer.lock = MUTEX_INITIALIZE();
    master->output_buffer.data_evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    master->output_buffer.empty_evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    master->output_buffer.write_evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);

    master->slave_idx = INT_MAX;

    *ptm_out = (dev_desc)master;

    return OBOS_STATUS_SUCCESS;
}

obos_status VfsH_GetPTS(dev_desc ptm, dirent** pts)
{
    if (!ptm)
        return OBOS_STATUS_INVALID_ARGUMENT;
    struct pty* master = (void*)ptm;
    *pts = master->slave;
    return OBOS_STATUS_SUCCESS;
}

obos_status VfsH_SetPTS(dev_desc ptm, dirent* node, int idx)
{
    if (!ptm || !node || idx == INT_MAX || idx < 0)
        return OBOS_STATUS_INVALID_ARGUMENT;
    struct pty* master = (void*)ptm;
    if (master->slave)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    
    master->slave_idx = idx;
    master->slave = node;

    return OBOS_STATUS_SUCCESS;
}

obos_status ptmx_get_blk_size(dev_desc desc, size_t* blkSize)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;

    *blkSize = 1;
    return OBOS_STATUS_SUCCESS;
}

obos_status ptmx_get_max_blk_count(dev_desc desc, size_t* count)
{
    OBOS_UNUSED(desc && count);
    return OBOS_STATUS_INVALID_OPERATION;
}

static obos_status ptmx_read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    OBOS_UNUSED(blkOffset);

    if (!desc || desc == 0x1)
        return OBOS_STATUS_INVALID_ARGUMENT;

    pty* pty = (void*)desc;
    if (blkCount > sizeof(pty->output_buffer.buffer))
        blkCount = sizeof(pty->output_buffer.buffer);

    while ((pty->output_buffer.ptr - pty->output_buffer.in_ptr) < (intptr_t)blkCount)
    {
        Core_MutexAcquire(&pty->output_buffer.lock);
        OBOS_ENSURE(pty->output_buffer.in_ptr <= pty->output_buffer.ptr);
        blkCount = (pty->output_buffer.ptr - pty->output_buffer.in_ptr);
        Core_MutexRelease(&pty->output_buffer.lock);
    }
    pty_output_read(pty, buf, blkCount, nBlkRead, false);
    
    if (nBlkRead)
        *nBlkRead = blkCount;
    return OBOS_STATUS_SUCCESS;
}

obos_status ptmx_write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    OBOS_UNUSED(blkOffset);
    if (!desc || desc == 0x1)
        return OBOS_STATUS_INVALID_ARGUMENT;
    struct pty* ptm = (void*)desc;
    if (!ptm->data_ready)
        return OBOS_STATUS_INTERNAL_ERROR;
    ptm->data_ready(ptm->tty, buf, blkCount);
    *nBlkWritten = blkCount;
    return OBOS_STATUS_SUCCESS;
}

static obos_status ptmx_submit_irp(void* /* irp* */ request)
{
    irp* req = request;
    if (!req) return OBOS_STATUS_INVALID_ARGUMENT;
    if (req->op == IRP_WRITE)
    {
        req->evnt = nullptr;
        req->on_event_set = nullptr;
        return OBOS_STATUS_SUCCESS;
    }

    struct pty* ptm = (void*)req->desc;
    if (!ptm || (req->desc == 0x1))
    {
        req->status = OBOS_STATUS_INVALID_ARGUMENT;
        return OBOS_STATUS_SUCCESS;
    }

    req->evnt = &ptm->output_buffer.data_evnt;
    req->on_event_set = nullptr;

    return OBOS_STATUS_SUCCESS;
}

static obos_status ptmx_finalize_irp(void* /* irp* */ request)
{
    irp* req = request;
    if (!req) return OBOS_STATUS_INVALID_ARGUMENT;

    if (req->op == IRP_WRITE)
    {
        if (req->dryOp) return OBOS_STATUS_SUCCESS;
        return ptmx_write_sync(req->desc, req->cbuff, req->blkCount, req->blkOffset, &req->nBlkWritten);
    }

    if (req->dryOp) return OBOS_STATUS_SUCCESS;

    return ptmx_read_sync(req->desc, req->buff, req->blkCount, req->blkOffset, &req->nBlkRead);
}

static obos_status ptmx_reference_device(dev_desc* desc)
{
    if ((*desc) == 1)
    {
        // This is /dev/ptmx
        obos_status status = VfsH_MakePTM(desc);
        if (obos_is_error(status))
            return status;
        pty* master = (void*)(*desc);
        master->master_refs++;
        printf("referencing PTS %p master, now at %d master refs, %d refs\n", master, master->master_refs, master->ptr.refs);
        tty_interface iface = Vfs_PTSInterface;
        iface.userdata = master;
        return Vfs_RegisterTTY(&iface, &master->slave, true);
    }

    pty* master = (void*)(*desc);
    OBOS_SharedPtrRef(&master->ptr);
    master->master_refs++;
    printf("referencing PTS %p master, now at %d master refs, %d refs\n", master, master->master_refs, master->ptr.refs);

    return OBOS_STATUS_SUCCESS;
}

static obos_status ptmx_unreference_device(dev_desc desc)
{
    OBOS_ASSERT(desc && desc != 0x1);
    if (!desc || desc == 0x1)
        return OBOS_STATUS_INVALID_ARGUMENT;

    pty* master = (void*)desc;
    
    master->master_refs--;
    printf("dereferencing PTS %p master, now at %d master refs, %d refs\n", master, master->master_refs, master->ptr.refs-1);
    if (!master->master_refs && master->ptr.refs > 1)
    {
        process* session_leader = master->slave->vnode->tty->session ? master->slave->vnode->tty->session->leader : nullptr;

        if (!session_leader)
            OBOS_KillProcessGroup(master->slave->vnode->tty->fg_job, SIGHUP);
        else
            OBOS_KillProcess(session_leader, SIGHUP);
        master->slave->vnode->tty->hang = true;
        Core_EventSet(&master->slave->vnode->tty->data_ready_evnt, false);
    }

    OBOS_SharedPtrUnref(&master->ptr);

    return OBOS_STATUS_SUCCESS;
}

#define TIOCGPTN 0x80045430U
#define TIOCSPTLCK 0x40045431U

static obos_status ptmx_ioctl(dev_desc what, uint32_t request, void* argp)
{
    if (!what || what == 0x1)   
        return OBOS_STATUS_INVALID_ARGUMENT;

    pty* master = (void*)what;

    switch (request) {
        case TIOCGPTN:
            *(int*)argp = master->slave_idx;
            return OBOS_STATUS_SUCCESS;
        case TIOCSPTLCK:
            if (*(int*)argp)
                master->slave->vnode->flags |= VFLAGS_PTS_LOCKED;
            else
                master->slave->vnode->flags &= ~VFLAGS_PTS_LOCKED;
            return OBOS_STATUS_SUCCESS;
        default: break;
    }

    return OBOS_STATUS_INVALID_IOCTL;
}

static obos_status ptmx_ioctl_argp_size(uint32_t request, size_t* ret)
{
    switch (request) {
        case TIOCGPTN:
        case TIOCSPTLCK:
            *ret = sizeof(int);
            return OBOS_STATUS_SUCCESS;
        default: break;
    }
    return OBOS_STATUS_INVALID_IOCTL;
}