/*
 * generic/libps2/main.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <vfs/irp.h>
#include <vfs/create.h>
#include <vfs/vnode.h>
#include <vfs/dirent.h>

#include <locks/event.h>

#include "controller.h"
#include "detect.h"

obos_status get_blk_size(dev_desc desc, size_t* blk_size)
{
    if (!desc || !blk_size)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ps2_port* port = (void*)desc;
    if (port->magic != PS2_PORT_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *blk_size = port->blk_size;
    return OBOS_STATUS_SUCCESS;
}

obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    OBOS_UNUSED(desc && count);
    return OBOS_STATUS_INVALID_OPERATION;
}

obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    OBOS_UNUSED(blkOffset);

    if (!desc || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ps2_port* port = (void*)desc;
    if (port->magic != PS2_PORT_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    uint8_t *out = buf;
    for (size_t i = 0; i < blkCount; i++)
        port->read_raw(port->default_handle, &out[i*port->blk_size], true);

    if (nBlkRead)
        *nBlkRead = blkCount;

    return OBOS_STATUS_SUCCESS;
}

obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    OBOS_UNUSED(desc && buf && blkCount && blkOffset && nBlkWritten);
    return OBOS_STATUS_INVALID_OPERATION;
}

obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata), void* userdata)
{
    if (PS2_GetPort(false) && PS2_GetPort(true)->read_raw)
        cb((dev_desc)PS2_GetPort(false), PS2_GetPort(false)->blk_size, 0, userdata);
    if (PS2_GetPort(true) && PS2_GetPort(true)->read_raw)
        cb((dev_desc)PS2_GetPort(true), PS2_GetPort(true)->blk_size, 0, userdata);
    return OBOS_STATUS_SUCCESS;
}

obos_status query_user_readable_name(dev_desc what, const char** name)
{
    if (!what || !name)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ps2_port* port = (void*)what;
    if (port->magic != PS2_PORT_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *name = port->str_id;
    return OBOS_STATUS_SUCCESS;
}

obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    if (!argp || !what)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ps2_port* port = (void*)what;
    if (port->magic != PS2_PORT_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    obos_status st = OBOS_STATUS_SUCCESS;
    switch (request)
    {
        case 1:
            st = port->get_readable_count(port->default_handle, argp);
            break;
        default:
            st = OBOS_STATUS_INVALID_IOCTL;
            break;
    }
    return st;
}

obos_status ioctl_argp_size(uint32_t request, size_t* res)
{
    obos_status st = OBOS_STATUS_SUCCESS;
    switch (request)
    {
        case 1:
            *res = sizeof(size_t);
            break;
        default:
            st = OBOS_STATUS_INVALID_IOCTL;
            break;
    }
    return st;
}

static void cleanup_port_vn(ps2_port* port)
{
    Vfs_UnlinkNode(port->ent);
    port->vn->flags |= VFLAGS_DRIVER_DEAD;
}

void cleanup()
{
    ps2_port* port1 = PS2_GetPort(false);    
    ps2_port* port2 = PS2_GetPort(false);
    if (port1)
        cleanup_port_vn(port1);
    if (port2)
        cleanup_port_vn(port2);
}

static void irp_event_set(irp* req)
{
    ps2_port* port = (void*)req->desc;
    size_t nReady = 0;
    port->get_readable_count(port->default_handle, &nReady);
    if (nReady >= req->blkCount)
        req->status = !req->dryOp ? read_sync(req->desc, req->buff, req->blkCount, 0, &req->nBlkRead) : OBOS_STATUS_SUCCESS;
    else
        req->status = OBOS_STATUS_IRP_RETRY;
    Core_EventClear(req->evnt);
}
obos_status submit_irp(void* req_)
{
    irp* req = req_;
    if (!req)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if ((!req->buff && !req->dryOp) || !req->refs || !req->desc)
        return OBOS_STATUS_INVALID_ARGUMENT;

    if (req->op == IRP_WRITE)
        return OBOS_STATUS_INVALID_OPERATION;

    ps2_port* port = (void*)req->desc;
    if (port->magic != PS2_PORT_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    size_t nReady = 0;
    port->get_readable_count(port->default_handle, &nReady);
    if (nReady >= req->blkCount)
    {
        req->evnt = nullptr;
        req->status = req->dryOp ? OBOS_STATUS_SUCCESS : read_sync(req->desc, req->buff, req->blkCount, 0, &req->nBlkRead);
    }
    else
    {
        req->on_event_set = irp_event_set;
        req->evnt = port->data_ready_event;
    }

    return OBOS_STATUS_SUCCESS;
}

OBOS_WEAK void on_suspend();
OBOS_WEAK void on_wake();

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_VERSION_FIELD|DRIVER_HEADER_HAS_STANDARD_INTERFACES,
    .ftable = {
        .driver_cleanup_callback = cleanup,
        .ioctl = ioctl,
        .ioctl_argp_size = ioctl_argp_size,
        .get_blk_size = get_blk_size,
        .get_max_blk_count = get_max_blk_count,
        .query_user_readable_name = query_user_readable_name,
        .foreach_device = foreach_device,
        .read_sync = read_sync,
        .write_sync = write_sync,
        .on_suspend = on_suspend,
        .on_wake = on_wake,
        .submit_irp = submit_irp,
    },
    .driverName = "PS/2 Driver",
    .version = 1,
    .mainThreadAffinity = 0b1,
};

OBOS_PAGEABLE_FUNCTION driver_init_status OBOS_DriverEntry(driver_id* this)
{
    ps2_port* port_one = PS2_GetPort(false);
    ps2_port* port_two = PS2_GetPort(true);
    for (size_t i = 0; i < 2; i++)
    {
        ps2_port* port = (i == 1) ? port_two : port_one;
        if (!port)
            continue;
        if (!port->works)
            continue;
        PS2_DetectDevice(port);
        if (port->type == PS2_DEV_TYPE_UNKNOWN)
            continue;
        vnode* vn = Drv_AllocateVNode(this, (uintptr_t)port, 0, nullptr, VNODE_TYPE_CHR);
        port->vn = vn;
        char dev_name[6] = {};
        memcpy(dev_name, port->str_id, 5);
        dev_name[4] = i == 0 ? '1' : '2';
        dev_name[5] = 0;
        OBOS_Debug("%*s: Registering PS/2 Device at %s%c%s\n", strnlen(this->header.driverName, 64), this->header.driverName, OBOS_DEV_PREFIX, OBOS_DEV_PREFIX[sizeof(OBOS_DEV_PREFIX)-1] == '/' ? 0 : '/', dev_name);
        port->ent = Drv_RegisterVNode(vn, dev_name);
    }
    PS2_EnableDevices();
    return (driver_init_status){.status=OBOS_STATUS_SUCCESS,.fatal=false,.context=nullptr};
}