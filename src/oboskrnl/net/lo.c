/*
 * oboskrnl/net/lo.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>

#include <vfs/dirent.h>
#include <vfs/vnode.h>
#include <vfs/irp.h>

#include <net/eth.h>
#include <net/tables.h>

#include <locks/event.h>
#include <locks/mutex.h>

#include <driver_interface/driverId.h>
#include <driver_interface/header.h>

#include <utils/list.h>

#include <allocators/base.h>

vnode* Net_LoopbackDevice = nullptr;

typedef struct lo_packet {
    void* buffer;
    size_t size;
    size_t refs;
    LIST_NODE(lo_packet_queue, struct lo_packet) node;
} lo_packet;
typedef LIST_HEAD(lo_packet_queue, lo_packet) lo_packet_queue; 
LIST_GENERATE_STATIC(lo_packet_queue, lo_packet, node);
enum {
    LO_DEV_MAGIC = 0x54010de7,
    LO_HND_MAGIC = 0x54010de8,
};
typedef struct lo_dev {
    uint32_t magic;
    lo_packet_queue recv;
    event evnt;
    mutex lock;
    size_t refs;
} lo_dev;
typedef struct lo_hnd {
    uint32_t magic;
    lo_dev* dev;
    lo_packet* curr;
    size_t curr_offset;
} lo_hnd;
static obos_status get_blk_size(dev_desc desc, size_t* blkSize)
{
    OBOS_UNUSED(desc);
    *blkSize = 1;
    return OBOS_STATUS_SUCCESS;
}

static obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    OBOS_UNUSED(desc && count);
    return OBOS_STATUS_INVALID_OPERATION;
}

static obos_status submit_irp(void* req_)
{
    irp* req = req_;
    if (!req)
        return OBOS_STATUS_INVALID_ARGUMENT;
    lo_hnd* hnd = (void*)req->desc;
    if (!hnd)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (hnd->magic != LO_HND_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    lo_dev* dev = hnd->dev;
    req->evnt = req->op == IRP_READ ? &dev->evnt : nullptr;
    req->on_event_set = nullptr;
    req->status = OBOS_STATUS_SUCCESS;
    return OBOS_STATUS_SUCCESS;
}
static obos_status finalize_irp(void* req_)
{
    irp* req = req_;
    if (!req)
        return OBOS_STATUS_INVALID_ARGUMENT;
    lo_hnd* hnd = (void*)req->desc;
    if (req->op == IRP_READ)
    {
        if (!hnd->curr)
        {
            Core_MutexAcquire(&hnd->dev->lock);
            hnd->curr = LIST_GET_HEAD(lo_packet_queue, &hnd->dev->recv);
            Core_MutexRelease(&hnd->dev->lock);
        }
        req->status = OBOS_STATUS_SUCCESS;
        if (req->dryOp)
        {
            req->nBlkRead = hnd->curr->size;
            return OBOS_STATUS_SUCCESS;
        }
        size_t szRead = OBOS_MIN(req->blkCount, hnd->curr->size - hnd->curr_offset);
        memcpy(req->buff, (char*)hnd->curr->buffer + hnd->curr_offset, szRead);
        hnd->curr_offset += szRead;
        if (hnd->curr_offset == hnd->curr->size)
        {
            if (!(--hnd->curr->refs))
            {
                lo_packet* del = hnd->curr;
                hnd->curr = LIST_GET_NEXT(lo_packet_queue, &hnd->dev->recv, hnd->curr);
                
                Core_MutexAcquire(&hnd->dev->lock);
                LIST_REMOVE(lo_packet_queue, &hnd->dev->recv, del);
                Core_MutexRelease(&hnd->dev->lock);

                Free(OBOS_KernelAllocator, del->buffer, del->size);
                Free(OBOS_KernelAllocator, del, sizeof(*del));
            }
        }
        
        req->nBlkRead = szRead;
    }
    else
    {
        req->status = OBOS_STATUS_SUCCESS;
        lo_packet* pckt = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(lo_packet), nullptr);
        pckt->buffer = Allocate(OBOS_KernelAllocator, req->blkCount, nullptr);
        memcpy(pckt->buffer, req->cbuff, req->blkCount);
        pckt->refs = hnd->dev->refs;
        pckt->size = req->blkCount;

        Core_MutexAcquire(&hnd->dev->lock);
        LIST_APPEND(lo_packet_queue, &hnd->dev->recv, pckt);
        Core_MutexRelease(&hnd->dev->lock);

        Core_EventSet(&hnd->dev->evnt, false);
    }
    return OBOS_STATUS_SUCCESS;
}

static obos_status ioctl_argp_size(uint32_t request, size_t *ret)
{
    switch (request) {
        case IOCTL_IFACE_MAC_REQUEST:
            *ret = sizeof(mac_address);
            return OBOS_STATUS_SUCCESS;
        default:
            return Net_InterfaceIoctlArgpSize(request, ret);
    }
    return OBOS_STATUS_INVALID_IOCTL;
}
static obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    if (!what) return OBOS_STATUS_INVALID_ARGUMENT;
    switch (request) {
        case IOCTL_IFACE_MAC_REQUEST:
            memzero(argp, sizeof(mac_address));
            return OBOS_STATUS_SUCCESS;
        default:
            return Net_InterfaceIoctl(Net_LoopbackDevice, request, argp);
    }
    return OBOS_STATUS_INVALID_IOCTL;
}

static obos_status reference_device(dev_desc* pdesc)
{
    if (!pdesc) return OBOS_STATUS_INVALID_ARGUMENT;
    if (!*pdesc) return OBOS_STATUS_INVALID_ARGUMENT;
    lo_dev* dev = (void*)(*pdesc);
    Core_MutexAcquire(&dev->lock);
    dev->refs++;
    lo_hnd* hnd = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(lo_hnd), nullptr);
    hnd->curr = LIST_GET_TAIL(lo_packet_queue, &dev->recv);
    hnd->magic = LO_HND_MAGIC;
    hnd->dev = dev;
    Core_MutexRelease(&dev->lock);
    *pdesc = (dev_desc)hnd;
    return OBOS_STATUS_SUCCESS;
}
static obos_status unreference_device(dev_desc desc)
{
    lo_hnd* hnd = (void*)desc;
    if (!hnd) return OBOS_STATUS_INVALID_ARGUMENT;
    // TODO: Unreference packets that haven't been received yet?
    hnd->dev->refs--;
    return OBOS_STATUS_SUCCESS;
}

driver_id OBOS_LoopbackDriver = {
    .id=0,
    .header = {
        .magic=OBOS_DRIVER_MAGIC,
        .driverName="Loopback Device Driver",
        .ftable = {
            .ioctl = ioctl,
            .ioctl_argp_size = ioctl_argp_size,
            .get_blk_size=get_blk_size,
            .get_max_blk_count=get_max_blk_count,
            .reference_device = reference_device,
            .unreference_device = unreference_device,
            .submit_irp = submit_irp,
            .finalize_irp = finalize_irp,
        },
    }
};

void Net_InitializeLoopbackDevice()
{
    lo_dev* dev = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(lo_dev), nullptr);
    dev->magic = LO_DEV_MAGIC;
    dev->evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    dev->lock = MUTEX_INITIALIZE();
    dev->refs = 0;
    Net_LoopbackDevice = Drv_AllocateVNode(&OBOS_LoopbackDriver, (dev_desc)dev, 0, nullptr, VNODE_TYPE_CHR);
    Drv_RegisterVNode(Net_LoopbackDevice, "lo");
}