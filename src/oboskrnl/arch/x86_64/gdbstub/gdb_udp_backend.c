/*
 * oboskrnl/arch/x86_64/gdbstub/gdb_udp_backend.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include "allocators/base.h"
#include "irq/dpc.h"
#include "scheduler/thread.h"
#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <vfs/vnode.h>

#include <arch/x86_64/gdbstub/alloc.h>
#include <arch/x86_64/gdbstub/connection.h>
#include <arch/x86_64/gdbstub/gdb_udp_backend.h>
#include <arch/x86_64/gdbstub/debug.h>

#include <driver_interface/header.h>

#include <net/udp.h>
#include <net/ip.h>
#include <net/tables.h>

#include <utils/list.h>

#include <vfs/dirent.h>
#include <vfs/vnode.h>
#include <vfs/alloc.h>

static obos_status get_blk_size(dev_desc desc, size_t* blkSize)
{
    OBOS_UNUSED(desc);
    if (!blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *blkSize = 1;
    return OBOS_STATUS_SUCCESS;
}

static obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    OBOS_UNUSED(desc);
    OBOS_UNUSED(count);
    return OBOS_STATUS_INVALID_OPERATION;
}

struct udp_handle {
    #define UDP_HANDLE_MAGIC 0xAD70C43F
    uint32_t magic; 
    
    vnode* interface;
    
    ip_addr client;
    uint16_t client_port;
    
    udp_queue* bound_port;
    ip_table_entry* table_ent;

    frame* curr_rx;
    size_t rx_off;

    gdb_connection* con;
};

static obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    OBOS_UNUSED(blkOffset);
    if (!buf || !desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    struct udp_handle* hnd = (struct udp_handle*)desc;
    if (hnd->magic != UDP_HANDLE_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    while (!hnd->curr_rx)
    {
        // printf(__FILE__ ":%d\n", __LINE__);
        Core_WaitOnObject(WAITABLE_OBJECT(hnd->bound_port->recv_event));
        // printf(__FILE__ ":%d\n", __LINE__);
		Core_EventClear(&hnd->bound_port->recv_event);
		frame* recv_packet = LIST_GET_HEAD(frame_queue, &hnd->bound_port->queue);
        if (!hnd->client.addr)
            hnd->client.addr = recv_packet->source_ip;
        // printf(__FILE__ ":%d\n", __LINE__);
        if (hnd->client.addr != recv_packet->source_ip)
            continue; // Discard packets from other clients.
        // printf(__FILE__ ":%d\n", __LINE__);
        // printf("UDP: Got a packet from %d.%d.%d.%d:%d\nPacket:\n%.*s\n", 
        //     hnd->client.comp1, hnd->client.comp2, hnd->client.comp3, hnd->client.comp4,
        //     hnd->client_port,
        //     recv_packet->sz, recv_packet->buff);
        hnd->client_port = recv_packet->source_port;
        hnd->curr_rx = recv_packet;
        // printf(__FILE__ ":%d\n", __LINE__);
    }

    const size_t nRead = OBOS_MIN(blkCount, hnd->curr_rx->sz);
    memcpy(buf, hnd->curr_rx->buff+hnd->rx_off, nRead);
    hnd->rx_off += blkCount;
    
    if (hnd->rx_off >= hnd->curr_rx->sz)
    {
        frame* next = LIST_GET_NEXT(frame_queue, &hnd->bound_port->queue, hnd->curr_rx);
		LIST_REMOVE(frame_queue, &hnd->bound_port->queue, hnd->curr_rx);
        NetH_ReleaseSharedBuffer(hnd->curr_rx->base);
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, hnd->curr_rx, sizeof(*hnd->curr_rx));
        hnd->curr_rx = next;
        hnd->rx_off = 0;
    }

    if (nBlkRead)
        *nBlkRead = nRead;

    return OBOS_STATUS_SUCCESS;
}

static obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    OBOS_UNUSED(blkOffset);

    if (!buf || !desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    struct udp_handle* hnd = (struct udp_handle*)desc;
    if (hnd->magic != UDP_HANDLE_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;

    obos_status st = OBOS_STATUS_SUCCESS;
    
    udp_header* udp_hdr = nullptr;
    st = Net_FormatUDPPacket(&udp_hdr, buf, blkCount, hnd->bound_port->dest_port, hnd->client_port);
    if (obos_is_error(st))
        return st;
    ip_header* packet = nullptr;
    ip_addr dest = hnd->client;
    st = Net_FormatIPv4Packet(&packet, udp_hdr, host_to_be16(udp_hdr->length), IPv4_PRECEDENCE_ROUTINE, &hnd->table_ent->address, &dest, 64, 0x11, 0, true);
    if (obos_is_error(st))
    {
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, udp_hdr, host_to_be16(udp_hdr->length));
        return st;
    }
    Net_TransmitIPv4Packet(hnd->interface, packet);

    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, udp_hdr, host_to_be16(udp_hdr->length));
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, packet, host_to_be16(packet->packet_length));

    if (nBlkWritten)
        *nBlkWritten = blkCount;

    return st;
}

static void driver_cleanup_callback(){};
static obos_status ioctl(dev_desc what, uint32_t request, void* argp) { OBOS_UNUSED(what); OBOS_UNUSED(request); OBOS_UNUSED(argp); return OBOS_STATUS_INVALID_IOCTL; }

static driver_ftable ftable = {
    .get_blk_size = get_blk_size,
    .get_max_blk_count = get_max_blk_count,
    .write_sync = write_sync,
    .read_sync = read_sync,
    .ioctl = ioctl,
    .driver_cleanup_callback = driver_cleanup_callback,
};

obos_status Kdbg_ConnectionInitializeUDP(gdb_connection* conn, uint16_t bind_port, vnode* interface)
{
    if (!interface || !bind_port || !conn)
        return OBOS_STATUS_INVALID_ARGUMENT;
    struct udp_handle* handle = Kdbg_Calloc(1, sizeof(struct udp_handle));
    handle->interface = interface;
    handle->table_ent = LIST_GET_HEAD(ip_table, &interface->tables->table);
    handle->magic = UDP_HANDLE_MAGIC;
    handle->con = conn;
    handle->bound_port = NetH_GetUDPQueueForPort(handle->table_ent, bind_port, true);
    return Kdbg_ConnectionInitialize(conn, &ftable, (dev_desc)handle);
}
