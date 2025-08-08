/*
 * oboskrnl/arch/x86_64/gdbstub/gdb_udp_backend.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <vfs/vnode.h>

#include <arch/x86_64/gdbstub/alloc.h>
#include <arch/x86_64/gdbstub/connection.h>
#include <arch/x86_64/gdbstub/gdb_udp_backend.h>
#include <arch/x86_64/gdbstub/debug.h>

#include <allocators/base.h>

#include <irq/dpc.h>

#include <scheduler/thread.h>

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
    
    udp_port* bound_port;
    ip_table_entry* table_ent;

    udp_recv_packet* curr_rx;
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
		udp_recv_packet* recv_packet = LIST_GET_HEAD(frame_queue, &hnd->bound_port->packets);
        if (!hnd->client.addr)
            hnd->client.addr = recv_packet->src.addr.addr;
        // printf(__FILE__ ":%d\n", __LINE__);
        if (hnd->client.addr != recv_packet->src.addr.addr)
            continue; // Discard packets from other clients.
        // printf(__FILE__ ":%d\n", __LINE__);
        // printf("UDP: Got a packet from %d.%d.%d.%d:%d\nPacket:\n%.*s\n", 
        //     hnd->client.comp1, hnd->client.comp2, hnd->client.comp3, hnd->client.comp4,
        //     hnd->client_port,
        //     recv_packet->sz, recv_packet->buff);
        hnd->client_port = recv_packet->src.port;
        hnd->curr_rx = recv_packet;
        // printf(__FILE__ ":%d\n", __LINE__);
    }

    const size_t nRead = OBOS_MIN(blkCount, hnd->curr_rx->buffer_ptr.szObj);
    memcpy(buf, hnd->curr_rx->buffer_ptr.obj + hnd->rx_off, nRead);
    hnd->rx_off += blkCount;
    
    if (hnd->rx_off >= hnd->curr_rx->buffer_ptr.szObj)
    {
        udp_recv_packet* next = LIST_GET_NEXT(udp_recv_packet_list, &hnd->bound_port->packets, hnd->curr_rx);
		OBOS_SharedPtrUnref(&hnd->curr_rx->packet_ptr);
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
    OBOS_UNUSED(blkOffset && blkCount && nBlkWritten);
    return OBOS_STATUS_UNIMPLEMENTED;
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
    handle->table_ent = LIST_GET_HEAD(ip_table, &interface->net_tables->table);
    handle->magic = UDP_HANDLE_MAGIC;
    handle->con = conn;
    Core_PushlockAcquire(&interface->net_tables->udp_ports_lock, true);
    udp_port key = {.port=bind_port};
    handle->bound_port = RB_FIND(udp_port_tree, &interface->net_tables->udp_ports, &key);
    Core_PushlockRelease(&interface->net_tables->udp_ports_lock, true);
    return Kdbg_ConnectionInitialize(conn, &ftable, (dev_desc)handle);
}
