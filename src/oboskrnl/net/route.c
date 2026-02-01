/*
 * oboskrnl/net/route.c
 *
 * Copyright (c) 2025-2026 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <syscall.h>
#include <perm.h>

#include <vfs/irp.h>
#include <vfs/mount.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <net/eth.h>
#include <net/tables.h>
#include <net/macros.h>
#include <net/icmp.h>
#include <net/ip.h>
#include <net/arp.h>

#include <locks/pushlock.h>

#include <utils/shared_ptr.h>
#include <utils/tree.h>
#include <utils/list.h>

#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>
#include <scheduler/process.h>
#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>

#include <allocators/base.h>

DefineNetFreeSharedPtr

static void dispatcher(vnode* nic)
{
    OBOS_Log("Entered network packet dispatcher in thread %d.%d\n", Core_GetCurrentThread()->proc->pid, Core_GetCurrentThread()->tid);
    net_tables* tables = nic->net_tables;
    obos_status status = OBOS_STATUS_SUCCESS;

    while (!tables->kill_dispatch)
    {
        irp* req = VfsH_IRPAllocate();
        req->blkCount = 0;
        req->blkOffset = 0;
        req->vn = nic;
        req->dryOp = true;
        req->op = IRP_READ;
        VfsH_IRPSubmit(req, &tables->desc);
        if (req->evnt && !Core_EventGetState(req->evnt))
            Net_TCPFlushACKs(tables);
        if (obos_is_error(status = VfsH_IRPWait(req)))
        {
            OBOS_Error("%s@%02x:%02x:%02x:%02x:%02x:%02x: VfsH_IRPWait: Status %d\n", 
                __func__,
                tables->mac[0], tables->mac[1], tables->mac[2], 
                tables->mac[3], tables->mac[4], tables->mac[5],
                status
            );
            VfsH_IRPUnref(req);
            break;
        }
        
        req->evnt = nullptr;
        req->drvData = nullptr;
        req->on_event_set = nullptr;
        req->blkCount = req->nBlkRead;
        req->nBlkRead = 0;
        req->buff = Allocate(OBOS_KernelAllocator, req->blkCount, nullptr);
        // req->blkCount = data->packet_size;
        req->dryOp = false;
        VfsH_IRPSubmit(req, &tables->desc);
        VfsH_IRPWait(req);
        // OBOS_ENSURE(req->nBlkRead == req->blkCount);

        shared_ptr* buf = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(shared_ptr), nullptr);
        OBOS_SharedPtrConstructSz(buf, req->buff, req->blkCount);
        buf->free = OBOS_SharedPtrDefaultFree;
        buf->onDeref = NetFreeSharedPtr;
        buf->freeUdata = OBOS_KernelAllocator;

        int depth = -1;
        InvokePacketHandler(Ethernet, buf->obj, buf->szObj, nullptr);
        
        VfsH_IRPUnref(req);
    }
    if (obos_is_error(status))
        OBOS_Error("%s@%02x:%02x:%02x:%02x:%02x:%02x: Aborting duing to previous failure. Status: %d\n", status);
    Core_ExitCurrentThread();
}

obos_status Net_Initialize(vnode* nic)
{
    if (!nic)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (nic->net_tables)
        return OBOS_STATUS_ALREADY_INITIALIZED;

    net_tables* tables = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(net_tables), nullptr);

    driver_header* driver = Vfs_GetVnodeDriver(nic);
    if (!driver)
        return OBOS_STATUS_INVALID_ARGUMENT;

    tables->desc = nic->desc;
    if (driver->ftable.reference_device)
        driver->ftable.reference_device(&tables->desc);

    driver->ftable.ioctl(tables->desc, IOCTL_IFACE_MAC_REQUEST, &tables->mac);

    if (~nic->flags & VFLAGS_NIC_PACKET_INJECT)
    {
        tables->dispatch_thread = CoreH_ThreadAllocate(nullptr);
        thread_ctx ctx = {};
        void* stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x4000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr);
        CoreS_SetupThreadContext(&ctx, (uintptr_t)dispatcher, (uintptr_t)nic, false, stack, 0x4000);
        tables->dispatch_thread->stackFree = CoreH_VMAStackFree;
        tables->dispatch_thread->stackFreeUserdata = &Mm_KernelContext;
        CoreH_ThreadInitialize(tables->dispatch_thread, THREAD_PRIORITY_REAL_TIME, Core_DefaultThreadAffinity, &ctx);
        Core_ProcessAppendThread(OBOS_KernelProcess, tables->dispatch_thread);
        CoreH_ThreadReady(tables->dispatch_thread);
    }

    tables->arp_cache_lock = PUSHLOCK_INITIALIZE();
    tables->table_lock = PUSHLOCK_INITIALIZE();
    tables->tcp_pending_acks.lock = MUTEX_INITIALIZE();
    tables->fragmented_packets_lock = PUSHLOCK_INITIALIZE();
    tables->udp_ports_lock = PUSHLOCK_INITIALIZE();
    tables->tcp_connections_lock = PUSHLOCK_INITIALIZE();
    tables->tcp_ports_lock = PUSHLOCK_INITIALIZE();
    tables->cached_routes_lock = PUSHLOCK_INITIALIZE();
    tables->interface = nic;
    tables->magic = IP_TABLES_MAGIC;

    nic->net_tables = tables;
    LIST_APPEND(network_interface_list, &Net_Interfaces, tables);

    return OBOS_STATUS_SUCCESS;
}
obos_status NetH_SendEthernetPacket(vnode *nic, shared_ptr* data)
{
    if (!nic || !data)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!nic->net_tables || !data->obj || !data->szObj)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (nic->net_tables->magic != IP_TABLES_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;

    irp* req = VfsH_IRPAllocate();
    req->vn = nic;
    VfsH_IRPBytesToBlockCount(nic, data->szObj, &req->blkCount);
    req->cbuff = data->obj;
    req->op = IRP_WRITE;
    req->blkOffset = 0;
    VfsH_IRPSubmit(req, &nic->net_tables->desc);
    VfsH_IRPWait(req);
    OBOS_SharedPtrUnref(data);
    VfsH_IRPUnref(req);
    return OBOS_STATUS_SUCCESS;
}

LIST_GENERATE(route_list, struct route, node);
RB_GENERATE(route_tree, route, rb_node, route_cmp);

obos_status NetH_AddressRoute(net_tables** interface, ip_table_entry** routing_entry, uint8_t* ttl, ip_addr destination)
{
    // Check local ip table entries and cached routes.
    for (net_tables* curr_iface = LIST_GET_HEAD(network_interface_list, &Net_Interfaces); curr_iface; )
    {
        Core_PushlockAcquire(&curr_iface->table_lock, true);
        for (ip_table_entry* ent = LIST_GET_HEAD(ip_table, &curr_iface->table); ent; )
        {
            if ((ent->address.addr & ent->subnet) == (destination.addr & ent->subnet))
            {
                *interface = curr_iface;
                *routing_entry = ent;
                *ttl = 64;
                Core_PushlockRelease(&curr_iface->table_lock, true);
                return OBOS_STATUS_SUCCESS;
            }
            
            ent = LIST_GET_NEXT(ip_table, &curr_iface->table, ent);
        }
        Core_PushlockRelease(&curr_iface->table_lock, true);

        Core_PushlockAcquire(&curr_iface->cached_routes_lock, true);
        struct route key = {.destination=destination,.iface=curr_iface};
        struct route* r = RB_FIND(route_tree, &curr_iface->cached_routes, &key);
        Core_PushlockRelease(&curr_iface->cached_routes_lock, true);
        if (r)
        {
            *interface = r->iface;
            *routing_entry = r->ent;
            *ttl = r->ttl;
            return OBOS_STATUS_SUCCESS;
        }

        curr_iface = LIST_GET_NEXT(network_interface_list, &Net_Interfaces, curr_iface);
    }
    
    route_list possible_routes = {};

    // Check routing table entries, adding possible routes to the list.
    for (net_tables* curr_iface = LIST_GET_HEAD(network_interface_list, &Net_Interfaces); curr_iface; )
    {
        for (gateway* ent = LIST_GET_HEAD(gateway_list, &curr_iface->gateways); ent; )
        {
            if (ent == curr_iface->default_gateway)
                goto end;
            if (ent->src.addr == destination.addr)
            {
                struct route* r = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(*r), nullptr);
                r->iface = curr_iface;
                r->ent = ent->dest_ent;
                r->route = ent;
                r->ttl = 60; /* initial TTL */
                LIST_APPEND(route_list, &possible_routes, r);
            }

            end:
            ent = LIST_GET_NEXT(gateway_list, &curr_iface->gateways, ent);
        }
        if (curr_iface->default_gateway)
        {
            struct route* r = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(*r), nullptr);
            r->iface = curr_iface;
            r->ent = curr_iface->default_gateway->dest_ent;
            r->route = curr_iface->default_gateway;
            r->ttl = 60; /* initial TTL */
            LIST_APPEND(route_list, &possible_routes, r);
        }

        curr_iface = LIST_GET_NEXT(network_interface_list, &Net_Interfaces, curr_iface);
    }

    struct route* optimal_route = nullptr;
    uint8_t optimal_route_hops = 0;

    if (!LIST_GET_NODE_COUNT(route_list, &possible_routes))
        goto done;
    else if (LIST_GET_NODE_COUNT(route_list, &possible_routes) == 1)
    {
        optimal_route = LIST_GET_HEAD(route_list, &possible_routes);
        goto done;
    }

    // Try each route, 
    for (struct route* curr = LIST_GET_HEAD(route_list, &possible_routes); curr; )
    {
        bool tried_again = false;
        again:
        (void)0;
        udp_port* port = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(udp_port), nullptr);
        port->port = 33435;
        port->recv_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
        Core_PushlockAcquire(&curr->iface->udp_ports_lock, false);
        RB_INSERT(udp_port_tree, &curr->iface->udp_ports, port);
        Core_PushlockRelease(&curr->iface->udp_ports_lock, false);
        udp_header hdr = {};
        hdr.dest_port = host_to_be16(33434);
        hdr.src_port = host_to_be16(port->port);
        hdr.length = host_to_be16(8);
        hdr.chksum = 0;
        shared_ptr data = {};
        OBOS_SharedPtrConstruct(&data, &hdr);
        NetH_SendIPv4Packet(curr->iface->interface, curr->ent, destination, 0x11, curr->ttl, 0, OBOS_SharedPtrCopy(&data));
        obos_status status = Core_WaitOnObject(WAITABLE_OBJECT(port->recv_event));
        if (obos_is_error(status))
            return status;

        icmp_header* icmp_hdr = port->icmp_header;
        shared_ptr* icmp_hdr_ptr = port->icmp_header_ptr;
        port->icmp_header_ptr = nullptr;

        Core_PushlockAcquire(&curr->iface->udp_ports_lock, false);
        RB_REMOVE(udp_port_tree, &curr->iface->udp_ports, port);
        Core_PushlockRelease(&curr->iface->udp_ports_lock, false);
        Free(OBOS_KernelAllocator, port, sizeof(*port));

        bool error = false;
        
        if (icmp_hdr)
        {
            if (icmp_hdr->type == ICMPv4_TYPE_TIME_EXCEEDED)
                error = true;
            else if (icmp_hdr->type == ICMPv4_TYPE_DEST_UNREACHABLE)
            {
                if (icmp_hdr->code == ICMPv4_CODE_PORT_UNREACHABLE || icmp_hdr->code == ICMPv4_CODE_PROTOCOL_UNREACHABLE || 
                    icmp_hdr->code == ICMPv4_CODE_COMMUNICATION_ADMINISTRATIVELY_FILTERED)
                {
                    ip_header* ip_hdr = (void*)icmp_hdr->data;
                    uint8_t hops = curr->ttl - ip_hdr->time_to_live;
                    if (!optimal_route || optimal_route_hops < hops)
                    {
                        optimal_route = curr;
                        optimal_route_hops = hops;
                    }
                }
                else
                    error = true;
            }
            
        }
        if (error)
        {
            if (!tried_again)
            {
                tried_again = true;
                curr->ttl *= 2;
                goto again;
            }
            else
            {
                struct route* next = LIST_GET_NEXT(route_list, &possible_routes, curr);
                LIST_REMOVE(route_list, &possible_routes, curr);
                Free(OBOS_KernelAllocator, curr, sizeof(*curr));
                curr = next;
                if (icmp_hdr_ptr)
                    OBOS_SharedPtrUnref(icmp_hdr_ptr);
                icmp_hdr_ptr = nullptr;
                continue;
            }
        }
        if (icmp_hdr_ptr)
            OBOS_SharedPtrUnref(icmp_hdr_ptr);
        icmp_hdr_ptr = nullptr;
        
        curr = LIST_GET_NEXT(route_list, &possible_routes, curr);
    }

    done:

    if (optimal_route)
    {
        *interface = optimal_route->iface;
        *routing_entry = optimal_route->ent;
        *ttl = optimal_route->ttl;
        optimal_route->hops = optimal_route_hops;
        Core_PushlockAcquire(&optimal_route->iface->cached_routes_lock, false);
        RB_FIND(route_tree, &optimal_route->iface->cached_routes, optimal_route);
        Core_PushlockRelease(&optimal_route->iface->cached_routes_lock, false);
    }

    for (struct route* curr = LIST_GET_HEAD(route_list, &possible_routes); curr; )
    {
        struct route* next = LIST_GET_NEXT(route_list, &possible_routes, curr);
        LIST_REMOVE(route_list, &possible_routes, curr);
        if (optimal_route != curr)
            Free(OBOS_KernelAllocator, curr, sizeof(*curr));
        curr = next;
    }

    if (optimal_route)
        return OBOS_STATUS_SUCCESS;
    else
        return OBOS_STATUS_NETWORK_UNREACHABLE;
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
obos_status NetH_GetLocalAddressInterface(net_tables** out_interface, ip_addr src)
{
    for (net_tables* interface = LIST_GET_HEAD(network_interface_list, &Net_Interfaces); interface; )
    {
        obos_status status = OBOS_STATUS_SUCCESS;
        status = interface_has_address(interface, src, nullptr);
        if (obos_is_error(status))
        {
            interface = LIST_GET_NEXT(network_interface_list, &Net_Interfaces, interface);
            continue;
        }
        *out_interface = interface;
        return OBOS_STATUS_SUCCESS;
    }
    return OBOS_STATUS_ADDRESS_NOT_AVAILABLE;
}

network_interface_list Net_Interfaces;

LIST_GENERATE(gateway_list, gateway, node);
LIST_GENERATE(ip_table, ip_table_entry, node);
LIST_GENERATE(network_interface_list, net_tables, node);
RB_GENERATE(address_table, address_table_entry, node, cmp_address_table_entry);

bool NetH_NetworkErrorLogsEnabled()
{
    static bool initialized_opt = 0, opt_val = 0;
    if (!initialized_opt)
    {
        opt_val = OBOS_GetOPTF("disable-network-error-logs");
        initialized_opt = true;
    }
    return opt_val;
}

static ip_table_entry* get_ip_table_entry(vnode* nic, ip_table_entry_user key)
{
    Core_PushlockAcquire(&nic->net_tables->table_lock, true);
    for (ip_table_entry* ent = LIST_GET_HEAD(ip_table, &nic->net_tables->table); ent; )
    {
        if (ent->address.addr == key.address.addr && ent->subnet == key.subnet)
        {
            Core_PushlockRelease(&nic->net_tables->table_lock, true);
            return ent;
        }

        ent = LIST_GET_NEXT(ip_table, &nic->net_tables->table, ent);
    }
    Core_PushlockRelease(&nic->net_tables->table_lock, true);
    return nullptr;
}
static gateway* get_gateway(vnode* nic, gateway_user key)
{
    for (gateway* ent = LIST_GET_HEAD(gateway_list, &nic->net_tables->gateways); ent; )
    {
        if (ent->dest.addr == key.dest.addr || ent->src.addr == key.src.addr)
            return ent;

        ent = LIST_GET_NEXT(gateway_list, &nic->net_tables->gateways, ent);
    }
    return nullptr;
}

obos_status Net_InterfaceIoctl(vnode* nic, uint32_t request, void* argp)
{
    if (!nic)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!nic->net_tables && request != IOCTL_IFACE_INITIALIZE)
        return OBOS_STATUS_UNINITIALIZED;
    obos_status status = OBOS_STATUS_SUCCESS;
    switch (request) {
        case IOCTL_IFACE_ADD_IP_TABLE_ENTRY:
        {
            status = OBOS_CapabilityCheck("net/ip-table-mod", false);
            if (obos_is_error(status))
                break;

            ip_table_entry_user* ent = argp;
            if (get_ip_table_entry(nic, *ent))
            {
                status = OBOS_STATUS_ALREADY_INITIALIZED;
                break;
            }
            ip_table_entry* new_ent = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(ip_table_entry), nullptr);
            new_ent->ip_entry_flags = ent->ip_entry_flags;
            new_ent->address = ent->address;
            new_ent->subnet = ent->subnet;
            new_ent->broadcast = ent->broadcast;
            Core_PushlockAcquire(&nic->net_tables->table_lock, false);
            LIST_APPEND(ip_table, &nic->net_tables->table, new_ent);
            Core_PushlockRelease(&nic->net_tables->table_lock, false);
            break;
        }
        case IOCTL_IFACE_REMOVE_IP_TABLE_ENTRY:
        {
            status = OBOS_CapabilityCheck("net/ip-table-mod", false);
            if (obos_is_error(status))
                break;

            ip_table_entry_user* key = argp;
            ip_table_entry* ent = get_ip_table_entry(nic, *key);
            if (!ent)
            {
                status = OBOS_STATUS_NOT_FOUND;
                break;
            }
            Core_PushlockAcquire(&nic->net_tables->table_lock, false);
            LIST_REMOVE(ip_table, &nic->net_tables->table, ent);
            Core_PushlockRelease(&nic->net_tables->table_lock, false);
            break;
        }
        case IOCTL_IFACE_SET_IP_TABLE_ENTRY:
        {
            status = OBOS_CapabilityCheck("net/ip-table-mod", false);
            if (obos_is_error(status))
                break;

            ip_table_entry_user* ent = argp;
            ip_table_entry* found = nullptr;
            if (!(found = get_ip_table_entry(nic, *ent)))
            {
                status = OBOS_STATUS_NOT_FOUND;
                break;
            }
            found->ip_entry_flags = ent->ip_entry_flags;
            found->address = ent->address;
            found->subnet = ent->subnet;
            found->broadcast = ent->broadcast;
            break;
        }
        case IOCTL_IFACE_ADD_ROUTING_TABLE_ENTRY:
        {
            status = OBOS_CapabilityCheck("net/routing-table-mod", false);
            if (obos_is_error(status))
                break;

            gateway_user *ent = argp;
            if (get_gateway(nic, *ent))
            {
                status = OBOS_STATUS_ALREADY_INITIALIZED;
                break;
            }
            if (!ent->src.addr)
            {
                status = OBOS_STATUS_INVALID_ARGUMENT;
                break;
            }
            Core_PushlockAcquire(&nic->net_tables->table_lock, true);
            ip_table_entry* dest_ent = nullptr;
            for (ip_table_entry* curr_ent = LIST_GET_HEAD(ip_table, &nic->net_tables->table); curr_ent; )
            {
                if ((curr_ent->address.addr & curr_ent->subnet) == (ent->dest.addr & curr_ent->subnet))
                {
                    dest_ent = curr_ent;
                    break;
                }

                curr_ent = LIST_GET_NEXT(ip_table, &nic->net_tables->table, curr_ent);
            }
            Core_PushlockRelease(&nic->net_tables->table_lock, true);
            if (!dest_ent)
            {
                status = OBOS_STATUS_NETWORK_UNREACHABLE;
                break;
            }
            gateway* new_ent = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(gateway), nullptr);
            new_ent->dest = ent->dest;
            new_ent->src = ent->src;
            new_ent->dest_ent = dest_ent;
            if (obos_is_success(status = NetH_ARPRequest(nic, new_ent->dest, nullptr, &new_ent->cache)))
                LIST_APPEND(gateway_list, &nic->net_tables->gateways, new_ent);
            else
                Free(OBOS_KernelAllocator, new_ent, sizeof(gateway));
            break;
        }
        case IOCTL_IFACE_REMOVE_ROUTING_TABLE_ENTRY:
        {
            status = OBOS_CapabilityCheck("net/routing-table-mod", false);
            if (obos_is_error(status))
                break;

            gateway_user* key = argp;
            gateway* ent = get_gateway(nic, *key);
            if (!ent)
            {
                status = OBOS_STATUS_NOT_FOUND;
                break;
            }
            LIST_REMOVE(gateway_list, &nic->net_tables->gateways, ent);
            break;
        }
        case IOCTL_IFACE_SET_DEFAULT_GATEWAY:
        {
            status = OBOS_CapabilityCheck("net/routing-table-mod", false);
            if (obos_is_error(status))
                break;

            ip_table_entry* dest_ent = nullptr;
            Core_PushlockAcquire(&nic->net_tables->table_lock, true);
            for (ip_table_entry* curr_ent = LIST_GET_HEAD(ip_table, &nic->net_tables->table); curr_ent; )
            {
                if ((curr_ent->address.addr & curr_ent->subnet) == (((ip_addr*)argp)->addr & curr_ent->subnet))
                {
                    dest_ent = curr_ent;
                    break;
                }

                curr_ent = LIST_GET_NEXT(ip_table, &nic->net_tables->table, curr_ent);
            }
            Core_PushlockRelease(&nic->net_tables->table_lock, true);
            if (!dest_ent)
            {
                status = OBOS_STATUS_NETWORK_UNREACHABLE;
                break;
            }

            gateway new_gateway = {};
            new_gateway.src = (ip_addr){.addr=0};
            new_gateway.dest = *(ip_addr*)argp;
            new_gateway.dest_ent = dest_ent;
            mac_address tmp = {};
            if (!nic->net_tables->default_gateway)
            {
                nic->net_tables->default_gateway = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(gateway), nullptr);
                LIST_APPEND(gateway_list, &nic->net_tables->gateways, nic->net_tables->default_gateway);
            }
            if (obos_is_success(status = NetH_ARPRequest(nic, new_gateway.dest, &tmp, &new_gateway.cache)))
                *nic->net_tables->default_gateway = new_gateway;
            break;
        }
        case IOCTL_IFACE_UNSET_DEFAULT_GATEWAY:
        {
            status = OBOS_CapabilityCheck("net/routing-table-mod", false);
            if (obos_is_error(status))
                break;

            if (nic->net_tables->default_gateway)
            {
                LIST_REMOVE(gateway_list, &nic->net_tables->gateways, nic->net_tables->default_gateway);
                nic->net_tables->default_gateway = nullptr;
                Free(OBOS_KernelAllocator, nic->net_tables->default_gateway, sizeof(*nic->net_tables->default_gateway));
            }
            status = OBOS_STATUS_SUCCESS;
            break;
        }
        case IOCTL_IFACE_CLEAR_ARP_CACHE:
        {
            status = OBOS_CapabilityCheck("net/clear-arp-cache", true);
            if (obos_is_error(status))
                break;

            address_table_entry *ent = nullptr, *next = nullptr;
            Core_PushlockAcquire(&nic->net_tables->arp_cache_lock, false);
            RB_FOREACH_SAFE(ent, address_table, &nic->net_tables->arp_cache, next)
            {
                RB_REMOVE(address_table, &nic->net_tables->arp_cache, ent);
                Core_EventSet(&ent->sync, false);
                Free(OBOS_KernelAllocator, ent, sizeof(*ent));
            }
            Core_PushlockRelease(&nic->net_tables->arp_cache_lock, false);
            break;
        }
        case IOCTL_IFACE_CLEAR_ROUTE_CACHE:
        {
            status = OBOS_CapabilityCheck("net/clear-route-cache", true);
            if (obos_is_error(status))
                break;

            struct route *ent = nullptr, *next = nullptr;
            Core_PushlockAcquire(&nic->net_tables->cached_routes_lock, false);
            RB_FOREACH_SAFE(ent, route_tree, &nic->net_tables->cached_routes, next)
            {
                RB_REMOVE(route_tree, &nic->net_tables->cached_routes, ent);
                Free(OBOS_KernelAllocator, ent, sizeof(*ent));
            }
            Core_PushlockRelease(&nic->net_tables->cached_routes_lock, false);
            break;
        }
        case IOCTL_IFACE_GET_IP_TABLE:
        {
            status = OBOS_CapabilityCheck("net/get-ip-table", true);
            if (obos_is_error(status))
                break;
            
            struct {
                void* buf;
                size_t sz;
            } *buffer = argp;
            
            Core_PushlockAcquire(&nic->net_tables->table_lock, true);
            size_t bytesCopied = 0, bytesToCopy = LIST_GET_NODE_COUNT(ip_table, &nic->net_tables->table)*sizeof(ip_table_entry_user);
            if (buffer->buf && buffer->sz >= sizeof(ip_table_entry_user))
            {
                char* kbuf = Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, buffer->buf, nullptr, buffer->sz, 0, true, &status);
                if (obos_is_error(status))
                    break;
                for (ip_table_entry* curr_ent = LIST_GET_HEAD(ip_table, &nic->net_tables->table); curr_ent && bytesCopied < buffer->sz; )
                {
                    ip_table_entry_user user_ent = {};
                    user_ent.ip_entry_flags = curr_ent->ip_entry_flags;
                    user_ent.address = curr_ent->address;
                    user_ent.subnet = curr_ent->subnet;
                    user_ent.broadcast = curr_ent->broadcast;
                    memcpy(kbuf+bytesCopied, &user_ent, sizeof(user_ent));
                    bytesCopied += sizeof(user_ent);
    
                    curr_ent = LIST_GET_NEXT(ip_table, &nic->net_tables->table, curr_ent);
                }
                Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, buffer->sz);
            }
            buffer->sz = bytesToCopy;
            Core_PushlockRelease(&nic->net_tables->table_lock, true);
            break;
        }
        case IOCTL_IFACE_GET_ROUTING_TABLE:
        {
            status = OBOS_CapabilityCheck("net/get-routing-table", true);
            if (obos_is_error(status))
                break;
            
            struct {
                void* buf;
                size_t sz;
            } *buffer = argp;
            
            size_t bytesCopied = 0, bytesToCopy = LIST_GET_NODE_COUNT(gateway_list, &nic->net_tables->gateways)*sizeof(gateway_user);
            if (buffer->buf && buffer->sz >= sizeof(gateway_user))
            {
                char* kbuf = Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, buffer->buf, nullptr, buffer->sz, 0, true, &status);
                if (obos_is_error(status))
                    break;
                for (gateway* curr_ent = LIST_GET_HEAD(gateway_list, &nic->net_tables->gateways); curr_ent && bytesCopied < buffer->sz; )
                {
                    gateway_user user_ent = {};
                    user_ent.dest = curr_ent->dest;
                    user_ent.src = curr_ent->src;
                    memcpy(kbuf+bytesCopied, &user_ent, sizeof(user_ent));
                    bytesCopied += sizeof(user_ent);
    
                    curr_ent = LIST_GET_NEXT(gateway_list, &nic->net_tables->gateways, curr_ent);
                }
                Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, buffer->sz);
            }
            buffer->sz = bytesToCopy;

            break;
        }
        case IOCTL_IFACE_INITIALIZE:
        {
            status = OBOS_CapabilityCheck("net/iface-start", false);
            if (obos_is_error(status))
                break;
            status = Net_Initialize(nic);
            break;
        }
        default:
            return OBOS_STATUS_INVALID_IOCTL;
    }
    return status;
}

obos_status Net_InterfaceIoctlArgpSize(uint32_t request, size_t* argp_sz)
{
    switch (request) {
        case IOCTL_IFACE_ADD_IP_TABLE_ENTRY:
        case IOCTL_IFACE_REMOVE_IP_TABLE_ENTRY:
        case IOCTL_IFACE_SET_IP_TABLE_ENTRY:
            *argp_sz = sizeof(ip_table_entry_user);
            break;
        case IOCTL_IFACE_ADD_ROUTING_TABLE_ENTRY:
        case IOCTL_IFACE_REMOVE_ROUTING_TABLE_ENTRY:
            *argp_sz = sizeof(gateway_user);
            break;
        case IOCTL_IFACE_CLEAR_ARP_CACHE:
        case IOCTL_IFACE_CLEAR_ROUTE_CACHE:
        case IOCTL_IFACE_UNSET_DEFAULT_GATEWAY:
        case IOCTL_IFACE_INITIALIZE:
            *argp_sz = 0;
            break;
        case IOCTL_IFACE_SET_DEFAULT_GATEWAY:
            *argp_sz = sizeof(ip_addr);
            break;
        case IOCTL_IFACE_GET_IP_TABLE:
        case IOCTL_IFACE_GET_ROUTING_TABLE:
            *argp_sz = sizeof(struct {void* buf; size_t sz;});
            break;
        default: *argp_sz = 0; return OBOS_STATUS_INVALID_IOCTL;
    }
    return OBOS_STATUS_SUCCESS;
}

string Net_Hostname;

obos_status Sys_GetHostname(char* name, size_t len)
{
    obos_status status = memcpy_k_to_usr(name, OBOS_GetStringCPtr(&Net_Hostname), OBOS_MIN(OBOS_GetStringSize(&Net_Hostname)+1, len));
    if (obos_is_error(status))
        return status;
    return len < OBOS_GetStringSize(&Net_Hostname)+1 ? OBOS_STATUS_INVALID_ARGUMENT : OBOS_STATUS_SUCCESS;
}

obos_status Sys_SetHostname(const char* uname, size_t len)
{
    if (len >= 64)
        return OBOS_STATUS_INVALID_ARGUMENT; // because of the obos dhcp client's theoeretical limits
    char name[65];
    memzero(name, sizeof(name));
    obos_status status = OBOSH_ReadUserString(uname, name, &len);
    if (obos_is_error(status))
        return status;
    OBOS_FreeString(&Net_Hostname);
    OBOS_InitStringLen(&Net_Hostname, name, len);
    OBOS_Log("NET: Changed hostname to %.*s\n", OBOS_GetStringSize(&Net_Hostname), OBOS_GetStringCPtr(&Net_Hostname));
    return OBOS_STATUS_SUCCESS;
}