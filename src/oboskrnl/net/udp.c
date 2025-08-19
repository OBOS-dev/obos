/*
 * oboskrnl/net/udp.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <net/macros.h>
#include <net/udp.h>
#include <net/ip.h>
#include <net/tables.h>
#include <net/icmp.h>

#include <vfs/socket.h>

#include <allocators/base.h>

#include <utils/list.h>

#include <scheduler/thread.h>
#include <scheduler/process.h>
#include <scheduler/schedule.h>
#include <scheduler/thread_context_info.h>

#if __x86_64__
#   include <arch/x86_64/gdbstub/connection.h>
#   include <arch/x86_64/gdbstub/debug.h>
#   include <mm/context.h>
#   include <mm/alloc.h>
static void kdbg_breaker_thread()
{
    Kdbg_Break();
    Core_ExitCurrentThread();
}
#endif

DefineNetFreeSharedPtr

#include <scheduler/cpu_local.h>

static void pckt_onDeref(struct shared_ptr* ptr)
{
    udp_recv_packet* pckt = ptr->obj;
    if (ptr->refs == 0)
    {
		OBOS_SharedPtrUnref(&pckt->buffer_ptr);
        LIST_REMOVE(udp_recv_packet_list, &pckt->bound_to->packets, pckt);
    }
}

PacketProcessSignature(UDP, ip_header*)
{
    OBOS_UNUSED(depth && size);
    udp_header* hdr = ptr;
    ip_header* ip_hdr = userdata;
    void* udp_pckt_data = hdr+1;
    size_t udp_pckt_sz = be16_to_host(hdr->length) - sizeof(udp_header);
    udp_port key = {.port=be16_to_host(hdr->dest_port)};
    Core_PushlockAcquire(&nic->net_tables->udp_ports_lock, false);
    udp_port* dest = RB_FIND(udp_port_tree, &nic->net_tables->udp_ports, &key);
    if (!dest)
    {
        Net_ICMPv4DestUnreachable(nic, ip_hdr, (ethernet2_header*)buf->obj, hdr, ICMPv4_CODE_PORT_UNREACHABLE);
        NetError("%s: UDP Port %d not bound to any socket.\n", __func__, key.port); 
        Core_PushlockRelease(&nic->net_tables->udp_ports_lock, false);
        ExitPacketHandler();
    }

    udp_recv_packet *pckt = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(udp_recv_packet), nullptr);
    
    OBOS_SharedPtrConstruct(&pckt->packet_ptr, pckt);
    pckt->packet_ptr.free = OBOS_SharedPtrDefaultFree;
    pckt->packet_ptr.freeUdata = OBOS_KernelAllocator;
    pckt->packet_ptr.onDeref = pckt_onDeref;
    OBOS_SharedPtrRef(&pckt->packet_ptr);
    
    OBOS_SharedPtrConstructSz(
        &pckt->buffer_ptr, 
        memcpy(Allocate(OBOS_KernelAllocator, udp_pckt_sz, nullptr), udp_pckt_data, udp_pckt_sz), 
        udp_pckt_sz);
    pckt->buffer_ptr.free = OBOS_SharedPtrDefaultFree;
    pckt->buffer_ptr.freeUdata = OBOS_KernelAllocator;
    OBOS_SharedPtrRef(&pckt->buffer_ptr);
    
    pckt->src.addr = ip_hdr->src_address;
    pckt->src.port = be16_to_host(hdr->src_port);

    pckt->bound_to = dest;
    
    LIST_APPEND(udp_recv_packet_list, &dest->packets, pckt);
    
    Core_EventSet(&dest->recv_event, false);
    
    Core_PushlockRelease(&nic->net_tables->udp_ports_lock, false);
    
    #if __x86_64__
        if (memcmp(udp_pckt_data, "\x03", 1) && udp_pckt_sz == 1)
        {
            // Kdbg break?
            if (Kdbg_CurrentConnection && Kdbg_CurrentConnection->connection_active && !Kdbg_Paused)
            {
                // We need to do this because this thread can't be blocked without
                // removing internet access, and because the idle thread cannot be
                // blocked.
                thread_ctx ctx = {};
                void* stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x4000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr);
                CoreS_SetupThreadContext(
                    &ctx,
                    (uintptr_t)kdbg_breaker_thread, 0, 
                    false, 
                    stack, 0x4000);
                thread* thr = CoreH_ThreadAllocate(nullptr);
                CoreH_ThreadInitialize(thr, THREAD_PRIORITY_NORMAL, Core_DefaultThreadAffinity, &ctx);
                Core_ProcessAppendThread(OBOS_KernelProcess, thr);
                thr->stackFree = CoreH_VMAStackFree;
                thr->stackFreeUserdata = &Mm_KernelContext;
                CoreH_ThreadReady(thr);
            }
        }
    #endif

    ExitPacketHandler();
}

LIST_GENERATE(udp_recv_packet_list, udp_recv_packet, node);
RB_GENERATE(udp_port_tree, udp_port, node, udp_port_cmp);

#include <vfs/alloc.h>

struct udp_bound_ports {
    udp_port** ports;
    size_t nPorts;
    event internal_read_event;
    event *read_event;
    udp_port* signaled_port;
    thread* internal_read_thread;
    event wake_read_thread;
    struct {
        ip_addr addr;
        uint16_t port;
    } default_peer;
    bool read_closed : 1;
    bool write_closed : 1;
};

socket_desc* udp_create()
{
    socket_desc* ret = Vfs_Calloc(1, sizeof(socket_desc));
    ret->ops = &Net_UDPSocketBackend;
    ret->protocol = IPPROTO_UDP;
    ret->protocol_data = nullptr;
    return ret;
}

static void udp_free(socket_desc* socket)
{
    OBOS_ASSERT(!socket->refs);
    struct udp_bound_ports *ports = socket->protocol_data;
    if (!ports)
        goto uninit;
    ports->read_closed = true;
    ports->write_closed = true;
    if (ports->internal_read_thread)
    {
        while (~ports->internal_read_thread->flags & THREAD_FLAGS_DIED)
            OBOSS_SpinlockHint();
        if (!(--ports->internal_read_thread->references) && ports->internal_read_thread->free)
            ports->internal_read_thread->free(ports->internal_read_thread);
    }
    for (size_t i = 0; i < ports->nPorts; i++)
    {
        udp_port* const port = ports->ports[i];
        if (port)
        {
            Core_PushlockAcquire(&port->iface->udp_ports_lock, false);
            RB_REMOVE(udp_port_tree, &port->iface->udp_ports, port);
            Core_PushlockRelease(&port->iface->udp_ports_lock, false);
            for (udp_recv_packet* curr = LIST_GET_HEAD(udp_recv_packet_list, &port->packets); curr; )
            {
                udp_recv_packet* const next = LIST_GET_NEXT(udp_recv_packet_list, &port->packets, curr);
                OBOS_SharedPtrUnref(&curr->packet_ptr);
                curr = next;
            }
            CoreH_AbortWaitingThreads(WAITABLE_OBJECT(port->recv_event));
            Free(OBOS_KernelAllocator, port, sizeof(*port));
            ports->ports[i] = nullptr;
        }
    }
    Vfs_Free(ports->ports);
    Vfs_Free(ports);
    uninit:
    Vfs_Free(socket);
}

static obos_status bind_interface(uint16_t port, net_tables* interface, udp_port** out)
{
    udp_port key = {.port=port};
    udp_port *bport = nullptr;
    Core_PushlockAcquire(&interface->udp_ports_lock, false);
    if (RB_FIND(udp_port_tree, &interface->udp_ports, &key))
        return OBOS_STATUS_ADDRESS_IN_USE;
    bport = Vfs_Calloc(1, sizeof(udp_port));
    bport->port = port;
    bport->recv_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    bport->iface = interface;
    RB_INSERT(udp_port_tree, &interface->udp_ports, bport);
    Core_PushlockRelease(&interface->udp_ports_lock, false);
    *out = bport;
    return OBOS_STATUS_SUCCESS;
}
static obos_status interface_has_address(net_tables* interface, ip_addr addr, ip_table_entry** oent)
{
    Core_PushlockAcquire(&interface->table_lock, true);
    for (ip_table_entry* ent = LIST_GET_HEAD(ip_table, &interface->table); ent; )
    {
        if (ent->address.addr == addr.addr)
        {
            if (oent)
                *oent = ent;
            Core_PushlockRelease(&interface->table_lock, true);
            return OBOS_STATUS_SUCCESS;
        }
        ent = LIST_GET_NEXT(ip_table, &interface->table, ent);
    }
    Core_PushlockRelease(&interface->table_lock, true);
    return OBOS_STATUS_ADDRESS_NOT_AVAILABLE;
}

static void internal_read_thread(void* userdata)
{
    struct udp_bound_ports* ports = userdata;
    struct waitable_header** events = ZeroAllocate(OBOS_KernelAllocator, 1+ports->nPorts, sizeof(struct waitable_header*), nullptr);
    events[0] = WAITABLE_OBJECT(ports->wake_read_thread);
    for (size_t i = 1; i < (ports->nPorts+1); i++)
        events[i] = WAITABLE_OBJECT(ports->ports[i-1]->recv_event);
    while (!ports->read_closed)
    {
        struct waitable_header* signaled = nullptr;
        Core_WaitOnObjects(1+ports->nPorts, events, &signaled);
        if (signaled == WAITABLE_OBJECT(ports->wake_read_thread))
            break;
        for (size_t i = 0; i < ports->nPorts; i++)
        {
            if (WAITABLE_OBJECT(ports->ports[i]->recv_event) == signaled)
            {
                ports->signaled_port = ports->ports[i];
                Core_EventSet(&ports->internal_read_event, false);
                break;
            }
            else
                continue;
        }
    }
    Free(OBOS_KernelAllocator, events, 1+ports->nPorts);
    Core_ExitCurrentThread();
}

obos_status udp_bind(socket_desc* socket, struct sockaddr_in* addr)
{
    if (socket->protocol_data)
        return OBOS_STATUS_INVALID_ARGUMENT;
    uint16_t port = be16_to_host(addr->port);
    if (port == 33434) // used for traceroute, we wouldn't want to break that (probably)
        return OBOS_STATUS_PORT_IN_USE;
    if (!port)
        return OBOS_STATUS_INVALID_ARGUMENT;
    struct udp_bound_ports* ports = Vfs_Calloc(1, sizeof(*ports));
    ports->internal_read_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    ports->wake_read_thread = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    if (addr->addr.addr == 0)
    {
        ports->nPorts = LIST_GET_NODE_COUNT(network_interface_list, &Net_Interfaces);
        ports->ports = Vfs_Calloc(ports->nPorts, sizeof(udp_port*));
        size_t i = 0;
        for (net_tables* interface = LIST_GET_HEAD(network_interface_list, &Net_Interfaces); interface && i < ports->nPorts; i++)
        {
            obos_status status = bind_interface(port, interface, &ports->ports[i]);
            if (obos_is_error(status))
            {
                Vfs_Free(ports->ports);
                Vfs_Free(ports);
                return status;
            }
            interface = LIST_GET_NEXT(network_interface_list, &Net_Interfaces, interface);
        }
        
    }
    else
    {
        ports->nPorts = 1;
        ports->ports = Vfs_Calloc(ports->nPorts, sizeof(udp_port*));
        for (net_tables* interface = LIST_GET_HEAD(network_interface_list, &Net_Interfaces); interface; )
        {
            obos_status status = OBOS_STATUS_SUCCESS;
            status = interface_has_address(interface, addr->addr, nullptr);
            if (obos_is_error(status))
            {
                interface = LIST_GET_NEXT(network_interface_list, &Net_Interfaces, interface);
                continue;
            }

            status = bind_interface(port, interface, &ports->ports[0]);
            if (obos_is_error(status))
            {
                Vfs_Free(ports->ports);
                Vfs_Free(ports);
                return status;
            }
            ports->read_event = &ports->ports[0]->recv_event;
            break;
        }
        if (!ports->ports[0])
        {
            Vfs_Free(ports->ports);
            Vfs_Free(ports);
            return OBOS_STATUS_ADDRESS_NOT_AVAILABLE;
        }
    }

    if (ports->nPorts == 1)
        ports->read_event = &ports->ports[0]->recv_event;
    else
    {
        ports->read_event = &ports->internal_read_event;
        ports->internal_read_thread = CoreH_ThreadAllocate(nullptr);
        thread_ctx ctx = {};
        void* stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x1000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr);
        CoreS_SetupThreadContext(&ctx, (uintptr_t)internal_read_thread, (uintptr_t)ports, false, stack, 0x1000);
        CoreH_ThreadInitialize(ports->internal_read_thread, THREAD_PRIORITY_NORMAL, Core_DefaultThreadAffinity, &ctx);
        ports->internal_read_thread->stackFree = CoreH_VMAStackFree;
        ports->internal_read_thread->stackFreeUserdata = &Mm_KernelContext;
        Core_ProcessAppendThread(OBOS_KernelProcess, ports->internal_read_thread);
        CoreH_ThreadReady(ports->internal_read_thread);
    }

    return OBOS_STATUS_SUCCESS;
}

uintptr_t mt_random();

obos_status udp_connect(socket_desc* socket, struct sockaddr_in* addr)
{
    if (socket->protocol_data)
        return OBOS_STATUS_ALREADY_INITIALIZED;

    struct udp_bound_ports* ports = socket->protocol_data = Vfs_Calloc(1, sizeof(struct udp_bound_ports));
    ports->nPorts = 1;
    ports->default_peer.addr = addr->addr;
    ports->default_peer.port = be16_to_host(addr->port);
    ports->ports = Vfs_Calloc(1, sizeof(udp_port*));
    ports->internal_read_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    ports->wake_read_thread = EVENT_INITIALIZE(EVENT_NOTIFICATION);

    net_tables* source_interface = nullptr;
    ip_table_entry* source_entry = nullptr;
    obos_status status = NetH_AddressRoute(&source_interface, &source_entry, &socket->opts.ttl, addr->addr);
    if (obos_is_error(status))
        return status;

    ports->ports[0] = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(udp_port), nullptr);
    ports->ports[0]->recv_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    ports->ports[0]->iface = source_interface;
    ports->read_event = &ports->ports[0]->recv_event;
    Core_PushlockAcquire(&source_interface->udp_ports_lock, false);
    udp_port* found = nullptr;
    uint32_t i = 0;
    do {
        ports->ports[0]->port = mt_random() % 0x10000 + 1;
        found = RB_FIND(udp_port_tree, &source_interface->udp_ports,ports->ports[0]);
    } while(found && i++ < 0x10000);
    ports->read_event = &ports->ports[0]->recv_event;

    RB_INSERT(udp_port_tree, &source_interface->udp_ports, ports->ports[0]);

    Core_PushlockRelease(&source_interface->udp_ports_lock, false);

    return OBOS_STATUS_SUCCESS;
}

obos_status udp_irp_write(irp *req)
{
    socket_desc* socket = (void*)req->desc;
    struct udp_bound_ports *ports = socket->protocol_data;
    if (!req->socket_data && !ports)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!ports && req->socket_data)
    {
        obos_status status = udp_connect(socket, req->socket_data);
        if (obos_is_error(status))
            return status;
        ports = socket->protocol_data;
    }
    uint16_t dest_port_be16 = 0;
    ip_addr dest_addr = {};
    if (req->socket_data)
    {
        if (req->sz_socket_data < 8)
        {
            req->status = OBOS_STATUS_INVALID_ARGUMENT;
            req->evnt = nullptr;
            req->on_event_set = nullptr;
            return OBOS_STATUS_SUCCESS;
        }
        dest_port_be16 = ((struct sockaddr_in*)req->socket_data)->port;
        dest_addr = ((struct sockaddr_in*)req->socket_data)->addr;
    }
    else
    {
        dest_port_be16 = host_to_be16(ports->default_peer.port);
        dest_addr = ports->default_peer.addr;
    }
    udp_header* out = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(udp_header)+req->blkCount, nullptr);
    out->src_port = host_to_be16(ports->ports[0]->port);
    out->dest_port = dest_port_be16;
    out->length = host_to_be16(8 + req->blkCount);
    out->chksum = 0;
    memcpy(out+1, req->buff, req->blkCount);
    
    net_tables* iface = nullptr;
    ip_table_entry* ent = nullptr;
    uint8_t ttl = 0;
    req->status = NetH_AddressRoute(&iface, &ent, &ttl, dest_addr);
    if (obos_is_error(req->status))
    {
        Free(OBOS_KernelAllocator, out, be16_to_host(out->length));
        return OBOS_STATUS_SUCCESS;
    }
    shared_ptr *pckt = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(shared_ptr), nullptr);
    OBOS_SharedPtrConstructSz(pckt, out, be16_to_host(out->length));
    pckt->free = OBOS_SharedPtrDefaultFree;
    pckt->freeUdata = OBOS_KernelAllocator;
    pckt->onDeref = NetFreeSharedPtr;
    req->status = NetH_SendIPv4Packet(iface->interface, ent, dest_addr, 0x11, ttl, 0, OBOS_SharedPtrCopy(pckt));
    req->nBlkWritten = req->blkCount;
    return OBOS_STATUS_SUCCESS;
}

static void irp_event_set(irp* req)
{
    socket_desc* socket = (void*)req->desc;
    struct udp_bound_ports *ports = socket->protocol_data;
    udp_port* port = ports->signaled_port ? ports->signaled_port : ports->ports[0];
    if (port->got_icmp_msg)
    {
        // Parse the ICMP packet.
        req->status = NetH_ICMPv4ResponseToStatus(port->icmp_header);
        OBOS_SharedPtrUnref(port->icmp_header_ptr);
        port->icmp_header_ptr = nullptr;
        return;
    }
    udp_recv_packet* pckt = LIST_GET_HEAD(udp_recv_packet_list, &port->packets);
    memcpy(req->buff, pckt->buffer_ptr.obj, OBOS_MIN(req->blkCount, pckt->buffer_ptr.szObj));
    if (~req->socket_flags & MSG_PEEK)
    {
        OBOS_SharedPtrUnref(&pckt->packet_ptr);
        Core_EventClear(req->evnt);
    }
    req->nBlkRead = OBOS_MIN(req->blkCount, pckt->buffer_ptr.szObj);
    ports->signaled_port = nullptr;
    req->status = OBOS_STATUS_SUCCESS;
}

obos_status udp_irp_read(irp *req)
{
    socket_desc* socket = (void*)req->desc;
    struct udp_bound_ports *ports = socket->protocol_data;
    if (!ports)
        return OBOS_STATUS_UNINITIALIZED;
    OBOS_ENSURE(ports->read_event);
    req->on_event_set = irp_event_set;
    req->evnt = ports->read_event;
    return OBOS_STATUS_SUCCESS;
}

obos_status udp_submit_irp(irp* req)
{
    socket_desc* socket = (void*)req->desc;
    if (req->blkCount > 0x10000-8-sizeof(ip_header))
    {
        req->status = OBOS_STATUS_MESSAGE_TOO_BIG;
        return OBOS_STATUS_SUCCESS;
    }
    if (req->dryOp)
    {
        if (req->op == IRP_WRITE)
        {
            req->evnt = nullptr;
            req->on_event_set = nullptr;
            req->status = OBOS_STATUS_SUCCESS;
            return OBOS_STATUS_SUCCESS;
        }
        else
        {
            struct udp_bound_ports *ports = socket->protocol_data;
            if (!ports)
                return OBOS_STATUS_UNINITIALIZED;
            req->evnt = ports->read_event;
            req->on_event_set = nullptr;
            return OBOS_STATUS_SUCCESS;
        }
    }
    else
    {
        if (req->op == IRP_WRITE)
            return udp_irp_write(req);
        else
            return udp_irp_read(req);
    }
    return OBOS_STATUS_UNIMPLEMENTED;
}

obos_status udp_shutdown(socket_desc* desc, int how)
{
    if (!desc->protocol_data)
        return OBOS_STATUS_INVALID_ARGUMENT;
    struct udp_bound_ports* ports = desc->protocol_data;
    if (how == SHUT_RD)
        ports->read_closed = true;
    else if (how == SHUT_WR)
        ports->write_closed = true;
    else if (how == SHUT_RDWR)
    {
        ports->write_closed = true;
        ports->read_closed = true;
    }
    else
        return OBOS_STATUS_INVALID_ARGUMENT;

    return OBOS_STATUS_SUCCESS;
}

struct socket_ops Net_UDPSocketBackend = {
    .protocol = IPPROTO_UDP,
    .create = udp_create,
	.free = udp_free,
	.accept = nullptr,
	.bind = udp_bind,
	.connect = udp_connect,
	.getpeername = nullptr,
	.getsockname = nullptr,
	.listen = nullptr,
	.submit_irp = udp_submit_irp,
	.finalize_irp = nullptr,
	.shutdown = udp_shutdown,
	.sockatmark = nullptr
};