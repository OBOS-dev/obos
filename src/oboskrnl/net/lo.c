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

#if 0
#include <arch/x86_64/asm_helpers.h>
static void write_to_serial(const void* buf, size_t size)
{
    static bool initialized_serial = false;
    if (!initialized_serial)
    {
        fd file = {};
        Vfs_FdOpen(&file, "/dev/COM1", FD_OFLAGS_READ|FD_OFLAGS_WRITE);
        OBOS_ALIGNAS(sizeof(uintptr_t)) struct {
            uint32_t baudRate;
            uint32_t dataBits;
            uint32_t stopBits;
            uint32_t parityBit;
        } args = {
            .baudRate = 115200,
            .dataBits = 3, // EIGHT_DATABITS
            .parityBit = 0,
            .stopBits = 0,
        };
        Vfs_FdIoctl(&file, 0x5e01, &args);
        initialized_serial = true;
    }
    const char* cbuf = buf;
    for (size_t i = 0; i < size; i++)
        outb(0x3f8, cbuf[i]);
}

#include <arch/x86_64/cmos.h>

static long get_current_time()
{
    long current_time = 0;
    Arch_CMOSGetEpochTime(&current_time);
    return current_time;
}
#endif

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
            if (hnd->curr)
                req->nBlkRead = hnd->curr->size;
            return OBOS_STATUS_SUCCESS;
        }
        Core_EventClear(req->evnt);
        if (!hnd->curr)
        {
            req->nBlkRead = 0;
            return OBOS_STATUS_SUCCESS;
        }
        size_t szRead = OBOS_MIN(req->blkCount, hnd->curr->size - hnd->curr_offset);
        memcpy(req->buff, (char*)hnd->curr->buffer + hnd->curr_offset, szRead);
        hnd->curr_offset += szRead;
        if (hnd->curr_offset == hnd->curr->size)
        {
            lo_packet* prev = hnd->curr;
            hnd->curr = LIST_GET_NEXT(lo_packet_queue, &hnd->dev->recv, hnd->curr);
            hnd->curr_offset = 0;
            if (!(--prev->refs))
            {
                Core_MutexAcquire(&hnd->dev->lock);
                LIST_REMOVE(lo_packet_queue, &hnd->dev->recv, prev);
                Core_MutexRelease(&hnd->dev->lock);

                Free(OBOS_KernelAllocator, prev->buffer, prev->size);
                Free(OBOS_KernelAllocator, prev, sizeof(*prev));
            }
        }
        
        req->nBlkRead = szRead;
    }
    else
    {
        req->status = OBOS_STATUS_SUCCESS;
        if (req->dryOp) return OBOS_STATUS_SUCCESS;
        lo_packet* pckt = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(lo_packet), nullptr);
        pckt->buffer = Allocate(OBOS_KernelAllocator, req->blkCount, nullptr);
        memcpy(pckt->buffer, req->cbuff, req->blkCount);
        pckt->refs = hnd->dev->refs;
        pckt->size = req->blkCount;
        Core_MutexAcquire(&hnd->dev->lock);
        if (pckt)
            LIST_APPEND(lo_packet_queue, &hnd->dev->recv, pckt);
        Core_MutexRelease(&hnd->dev->lock);
        // printf("LO: TX Packet (size: %d)\n", pckt->size);

#if 0
        static bool lo_sent_file_header = false;
        if (!lo_sent_file_header)
        {
            struct {
                uint32_t magic_number;
                uint16_t minor;
                uint16_t major;
                uint32_t resv[2];
                uint32_t snap_len;
                uint16_t link_type;
                uint16_t flags;
            } OBOS_PACK hdr = {
                .magic_number = 0xA1B2C3D4,
                .major = 2,
                .minor = 4,
                .snap_len = 0x40000,
                .link_type = 1,
                .flags = 0x5000
            };
            write_to_serial(&hdr, sizeof(hdr));
            lo_sent_file_header = true;
        }

        struct {
            uint32_t tstamp_sec;
            uint32_t tstamp_usec;
            uint32_t captured_packet_length;
            uint32_t network_packet_length;
            char data[];
        } record = {};
        record.network_packet_length = pckt->size;
        record.captured_packet_length = record.network_packet_length;
        record.tstamp_sec = get_current_time();
        record.tstamp_usec = record.tstamp_sec * 1000000;
        
        write_to_serial(&record, sizeof(record));
        write_to_serial(pckt->buffer, pckt->size);
#endif

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
            ((uint8_t*)argp)[0] |= BIT(1);
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