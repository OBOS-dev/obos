/*
 * oboskrnl/net/eth.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <net/eth.h>
#include <net/frame.h>
#include <net/ip.h>
#include <net/arp.h>
#include <net/tables.h>

#include <locks/rw_lock.h>
#include <locks/event.h>

#include <irq/dpc.h>

#include <allocators/base.h>

#include <scheduler/thread.h>

#include <utils/list.h>

static bool initialized_crc32 = false;
static uint32_t crctab[256];

// For future reference, we cannot hardware-accelerate the crc32 algorithm as
// x86-64's crc32 uses a different polynomial than that of GPT.

static void crcInit()
{
    uint32_t crc = 0;
    for (uint16_t i = 0; i < 256; ++i)
    {
        crc = i;
        for (uint8_t j = 0; j < 8; ++j)
        {
            uint32_t mask = -(crc & 1);
            crc = (crc >> 1) ^ (0xEDB88320 & mask);
        }
        crctab[i] = crc;
    }
}
static uint32_t crc(const char *data, size_t len, uint32_t result)
{
    for (size_t i = 0; i < len; ++i)
        result = (result >> 8) ^ crctab[(result ^ data[i]) & 0xFF];
    return ~result;
}
static uint32_t crc32_bytes_from_previous(void *data, size_t sz,
                                   uint32_t previousChecksum)
{
    if (!initialized_crc32)
    {
        crcInit();
        initialized_crc32 = true;
    }
    return crc((char *)data, sz, ~previousChecksum);
}
static uint32_t crc32_bytes(void *data, size_t sz)
{
    if (!initialized_crc32)
    {
        crcInit();
        initialized_crc32 = true;
    }
    return crc((char *)data, sz, ~0U);
}

static OBOS_NO_UBSAN void set_checksum(ethernet2_header* hdr, size_t sz)
{
    *(uint32_t*)(&((uint8_t*)hdr)[sizeof(ethernet2_header) + sz]) = crc32_bytes(hdr, sizeof(ethernet2_header) + sz);
}

obos_status Net_FormatEthernet2Packet(ethernet2_header** phdr, void* data, size_t sz, const mac_address* restrict dest, const mac_address* restrict src, uint16_t type, size_t *out_sz)
{
    if (!phdr || !data || !sz || !src || !dest || !out_sz)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ethernet2_header* hdr = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(ethernet2_header)+sz+4, nullptr);
    memcpy(hdr->dest, dest, sizeof(mac_address));
    memcpy(hdr->src, src, sizeof(mac_address));
    hdr->type = host_to_be16(type);
    memcpy(hdr+1, data, sz);
    set_checksum(hdr, sz);
    *out_sz = sizeof(ethernet2_header)+sz+4;
    *phdr = hdr;
    return OBOS_STATUS_SUCCESS;
}

static frame_queue frames;

static void receive_packet(dpc* d, void* userdata)
{
    OBOS_UNUSED(d);
    frame* data = userdata;
    ethernet2_header* hdr = (void*)data->buff;
    // TODO: Do we need to see if the MAC address in the frame matches our MAC address?
    frame pkt_frame = *data;
    pkt_frame.buff += sizeof(*hdr);
    pkt_frame.base->refcount++;
    pkt_frame.sz -= sizeof(*hdr);
    // remove the checksum
    pkt_frame.sz -= 4;
    OBOS_Debug("%04x\n", be16_to_host(hdr->type));
    switch (be16_to_host(hdr->type))
    {
        case ETHERNET2_TYPE_IPv4:
        {
            Net_IPReceiveFrame(&pkt_frame);
            break;
        }
        case ETHERNET2_TYPE_ARP:
        {
            // OBOS_Debug("data.mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
            //            pkt_frame.source_mac_address[0], pkt_frame.source_mac_address[1], pkt_frame.source_mac_address[2],
            //            pkt_frame.source_mac_address[3], pkt_frame.source_mac_address[4], pkt_frame.source_mac_address[5]
            // );
            Net_ARPReceiveFrame(&pkt_frame);
            break;
        }
        default:
            break;
    }
    NetH_ReleaseSharedBuffer(data->base);
    LIST_REMOVE(frame_queue, &frames, data);
    asm volatile ("" : : :"memory");
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, data, sizeof(*data));
}

static void queue_packet(void* buff, size_t sz, vnode* vnode, uint8_t(*mac)[6], net_shared_buffer* buffer)
{
    frame* data = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(frame), nullptr);
    data->buff = buff;
    data->interface_vn = vnode;
    memcpy(data->interface_mac_address, mac, 6);
    data->sz = sz;
    data->base = buffer;
    data->receive_dpc.userdata = data;
    // OBOS_Debug("initializing DPC\n");
    LIST_APPEND(frame_queue, &frames, data);
    receive_packet(nullptr, data);
    // CoreH_InitializeDPC(&data->receive_dpc, receive_packet, Core_DefaultThreadAffinity);
}

static void data_ready(void* userdata, void* vn_, size_t bytes_ready)
{
    OBOS_UNUSED(userdata);
    vnode* vn = vn_;
    mac_address addr = {};
    vn->un.device->driver->header.ftable.ioctl(vn->desc, IOCTL_ETHERNET_INTERFACE_MAC_REQUEST, addr);
    void* buffer = OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, bytes_ready, nullptr);
    size_t read = 0, bytes_left = bytes_ready;
    net_shared_buffer* base = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(net_shared_buffer), nullptr);
    base->base = buffer;
    base->buff_size = bytes_ready;
    while (bytes_left)
    {
        vn->un.device->driver->header.ftable.read_sync(vn->desc, (void*)((uintptr_t)buffer+(bytes_ready-bytes_left)), bytes_left, 0, &read);
        queue_packet((void*)((uintptr_t)buffer+(bytes_ready-bytes_left)), read, vn, &addr, base);
        bytes_left -= read;
    }
}

obos_status Net_EthernetUp(vnode* interface_vn)
{
    if (!interface_vn)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (interface_vn->vtype != VNODE_TYPE_CHR)
        return OBOS_STATUS_INVALID_OPERATION;
    if (!interface_vn->un.device)
        return OBOS_STATUS_INVALID_OPERATION;
    if (!interface_vn->un.device->driver->header.ftable.set_data_ready_cb)
        return OBOS_STATUS_INVALID_OPERATION;
    if (interface_vn->un.device->driver->header.ftable.reference_interface)
        interface_vn->un.device->driver->header.ftable.reference_interface(interface_vn->desc);
    obos_status status = interface_vn->un.device->driver->header.ftable.set_data_ready_cb(interface_vn, data_ready, nullptr);
    if (obos_is_error(status))
        return status;
    tables* tables = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(struct tables), nullptr);
    tables->address_to_phys_lock = RWLOCK_INITIALIZE();
    tables->table_lock = RWLOCK_INITIALIZE();
    tables->gateway_lock = EVENT_INITIALIZE(EVENT_SYNC);
    Core_EventSet(&tables->gateway_lock, false);
    tables->magic = IP_TABLES_MAGIC;
    tables->interface = interface_vn;
    interface_vn->un.device->driver->header.ftable.ioctl(interface_vn->desc, IOCTL_ETHERNET_INTERFACE_MAC_REQUEST, &tables->interface_mac);
    interface_vn->data = (uint64_t)(uintptr_t)tables;
    return OBOS_STATUS_SUCCESS;
}

void NetH_ReleaseSharedBuffer(net_shared_buffer* buff)
{
    if (!(--buff->refcount))
    {
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, buff->base, buff->buff_size);
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, buff, sizeof(*buff));
    }
}
