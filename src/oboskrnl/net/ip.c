/*
 * oboskrnl/net/ip.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <klog.h>

#include <net/macros.h>
#include <net/eth.h>
#include <net/ip.h>
#include <net/udp.h>
#include <net/tables.h>
#include <net/icmp.h>
#include <net/arp.h>

#include <locks/pushlock.h>

#include <allocators/base.h>

#include <utils/shared_ptr.h>
#include <utils/list.h>

uint16_t NetH_OnesComplementSum(const void *buffer, size_t size)
{
    const uint16_t *p = buffer;
    int sum = 0;
    int i;
    for (i = 0; i < ((int)size & ~(1)); i += 2) {
        sum += be16_to_host(p[i >> 1]);
    }

    if (size & 1) {
        sum += ((uint8_t *)p)[i];
    }

    sum = (sum >> 16) + (sum & 0xffff);
    sum += sum >> 16;

    uint16_t ret = ~sum;
    return ret;
}

obos_status NetH_ResolveExternalIP(vnode* nic, ip_addr addr, mac_address* out)
{
    Core_PushlockAcquire(&nic->net_tables->table_lock, true);
    for (ip_table_entry* ent = LIST_GET_HEAD(ip_table, &nic->net_tables->table); ent; )
    {
        if ((addr.addr & ent->subnet) == (ent->address.addr & ent->subnet))
        {
            Core_PushlockRelease(&nic->net_tables->table_lock, true);
            return NetH_ARPRequest(nic, addr, out, nullptr);
        }

        ent = LIST_GET_NEXT(ip_table, &nic->net_tables->table, ent);
    }
    Core_PushlockRelease(&nic->net_tables->table_lock, true);

    // We need a gateway.
    for (gateway* ap = LIST_GET_HEAD(gateway_list, &nic->net_tables->gateways); ap; )
    {
        if (ap->src.addr == addr.addr)
        {
            if (ap->cache)
            {
                memcpy(out, ap->cache->phys, sizeof(mac_address));
                return OBOS_STATUS_SUCCESS;
            }
            else
                return NetH_ARPRequest(nic, ap->dest, out, &ap->cache);
        }

        ap = LIST_GET_NEXT(gateway_list, &nic->net_tables->gateways, ap);
    }
    gateway *const default_gateway = nic->net_tables->default_gateway;
    if (default_gateway->cache)
    {
        memcpy(out, default_gateway->cache->phys, sizeof(mac_address));
        return OBOS_STATUS_SUCCESS;
    }
    return NetH_ARPRequest(nic, default_gateway->dest, out, &default_gateway->cache);
}

static void fragmented_packet_onDeref(shared_ptr* This)
{
    unassembled_ip_packet *packet = This->obj;
    Core_PushlockAcquire(&packet->owner->fragmented_packets_lock, false);
    RB_REMOVE(unassembled_ip_packets, &packet->owner->fragmented_packets, packet);
    Core_PushlockRelease(&packet->owner->fragmented_packets_lock, false);
}

DefineNetFreeSharedPtr

PacketProcessSignature(IPv4, ethernet2_header*)
{
    ethernet2_header* eth = userdata;
    OBOS_UNUSED(depth);

    ip_header* hdr = ptr;
    uint16_t remote_checksum = hdr->chksum;
    uint16_t our_checksum = 0;
    hdr->chksum = 0;
    our_checksum = NetH_OnesComplementSum(ptr, IPv4_GET_HEADER_LENGTH(hdr));
    hdr->chksum = remote_checksum;
    remote_checksum = be16_to_host(remote_checksum);
    if (our_checksum != remote_checksum)
    {
        NetError("%s: Wrong IP checksum in packet from " MAC_ADDRESS_FORMAT ". Expected checksum is 0x%04x, remote checksum is 0x%04x\n",
            __func__,
            MAC_ADDRESS_ARGS(eth->src),
            our_checksum,
            remote_checksum
        );
        ExitPacketHandler();
    }
    if (be16_to_host(hdr->packet_length) > size)
    {
        NetError("%s: Invalid packet size in packet from " MAC_ADDRESS_FORMAT ". \"packet_length > real_size\".\n",
            __func__,
            MAC_ADDRESS_ARGS(eth->src));
        ExitPacketHandler();
    }

    bool destination_local = false;
    Core_PushlockAcquire(&nic->net_tables->table_lock, true);
    ip_table_entry* forwarding_entry = nullptr;
    for (ip_table_entry* ent = LIST_GET_HEAD(ip_table, &nic->net_tables->table); ent; )
    {
        if (ent->address.addr == hdr->dest_address.addr)
        {
            destination_local = true;
            break;
        }
        if (ent->ip_entry_flags & IP_ENTRY_IPv4_FORWARDING && !forwarding_entry)
            forwarding_entry = ent;

        ent = LIST_GET_NEXT(ip_table, &nic->net_tables->table, ent);
    }

    void *data = (void*)((uintptr_t)hdr + IPv4_GET_HEADER_LENGTH(hdr));
    size_t data_size = size - IPv4_GET_HEADER_LENGTH(hdr);

    if (!destination_local)
    {
        if (!forwarding_entry)
        {
            Net_ICMPv4DestUnreachable(nic, hdr, eth, data, ICMPv4_CODE_NET_UNREACHABLE);
            ExitPacketHandler();
        }

        if (hdr->time_to_live-- == 1)
        {
            Net_ICMPv4TimeExceeded(nic, hdr, eth, data, ICMPv4_CODE_TTL_EXCEEDED);
            ExitPacketHandler();
        }

        hdr->chksum = 0;
        hdr->chksum = be16_to_host(NetH_OnesComplementSum(hdr, IPv4_GET_HEADER_LENGTH(hdr)));

        size_t sz = be16_to_host(hdr->packet_length) + sizeof(ethernet2_header);
        char* pckt = Allocate(OBOS_KernelAllocator, sz, nullptr);

        ethernet2_header* eth_hdr = (void*)pckt;
        memcpy(eth_hdr, hdr, be16_to_host(hdr->packet_length));
        
        memcpy(eth_hdr->src, nic->net_tables->mac, sizeof(mac_address));
        obos_status status = NetH_ResolveExternalIP(nic, hdr->dest_address, &eth_hdr->dest);
        if (obos_is_error(status))
        {
            Net_ICMPv4DestUnreachable(nic, hdr, eth, data, ICMPv4_CODE_NET_UNREACHABLE);
            Free(OBOS_KernelAllocator, pckt, sz);
            ExitPacketHandler();
        }
        eth_hdr->type = host_to_be16(ETHERNET2_TYPE_IPv4);

        shared_ptr *data_ptr = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(shared_ptr), nullptr);
        OBOS_SharedPtrConstructSz(data_ptr, pckt, sz);
        data_ptr->free = OBOS_SharedPtrDefaultFree;
        data_ptr->freeUdata = OBOS_KernelAllocator;
        data_ptr->onDeref = NetFreeSharedPtr;

        NetH_SendEthernetPacket(nic, OBOS_SharedPtrCopy(data_ptr));
        // pckt is freed once the packet is sent

        ExitPacketHandler();
    }

    if (be32_to_host((hdr)->id_flags_fragment) & IPv4_MORE_FRAGMENTS || 
        IPv4_GET_FRAGMENT(hdr)
        )
    {
        // TODO: How to know we actually received all fragments?
        NetUnimplemented(IPv4_FRAGMENTATION);
        ExitPacketHandler();

        uint16_t id = IPv4_GET_ID(hdr);
        unassembled_ip_packet what = {.id=id,.src=hdr->src_address};
        Core_PushlockAcquire(&nic->net_tables->fragmented_packets_lock, true);
        unassembled_ip_packet* packet = RB_FIND(unassembled_ip_packets, &nic->net_tables->fragmented_packets, &what);
        Core_PushlockRelease(&nic->net_tables->fragmented_packets_lock, true);
        if (!packet)
        {
            packet = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(unassembled_ip_packet), nullptr);
            packet->real_id = what.real_id;
            packet->owner = nic->net_tables;
            OBOS_SharedPtrConstruct(&packet->This, packet);
            packet->This.onDeref = fragmented_packet_onDeref;
            Core_PushlockAcquire(&nic->net_tables->fragmented_packets_lock, false);
            RB_INSERT(unassembled_ip_packets, &nic->net_tables->fragmented_packets, &what);
            Core_PushlockRelease(&nic->net_tables->fragmented_packets_lock, false);
            OBOS_SharedPtrRef(&packet->This);
        }
        OBOS_SharedPtrRef(&packet->This);
        ip_fragment *new_fragment = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(ip_fragment), nullptr);
        OBOS_SharedPtrRef(buf);
        new_fragment->hdr = hdr;
        new_fragment->hdr_ptr = buf;
        new_fragment->offset = IPv4_GET_FRAGMENT(hdr) * 8;
        if (packet->highest_offset < new_fragment->offset)
        {
            packet->highest_offset = new_fragment->offset;
            packet->size = new_fragment->offset + new_fragment->hdr->packet_length - sizeof(ip_header);
        }
        LIST_APPEND(ip_fragments, &packet->fragments, new_fragment);
        if (~be32_to_host((hdr)->id_flags_fragment) & IPv4_MORE_FRAGMENTS)
        {
            shared_ptr assembled_packet = NetH_IPv4ReassemblePacket(nic, packet);
            Net_IPv4Process(nic, depth + 1, OBOS_SharedPtrCopy(&assembled_packet), assembled_packet.obj, assembled_packet.szObj, userdata);
            OBOS_SharedPtrUnref(&assembled_packet);
            // At this point, all fragments have been freed.
            // See fragmented_packet_onDeref for why we don't
            // remove the packet from the RB-tree.
            OBOS_SharedPtrUnref(&packet->This);
        }
        ExitPacketHandler();
    }

    switch (hdr->protocol) {
        case 0x11:
            InvokePacketHandler(UDP, data, data_size, hdr);
            break;
        case 0x01:
            InvokePacketHandler(ICMPv4, data, data_size, hdr);
            break;
        default:
            Net_ICMPv4DestUnreachable(nic, hdr, eth, data, ICMPv4_CODE_PROTOCOL_UNREACHABLE);
            NetError("%s: Unrecognized IP protocol type 0x%02x from " IP_ADDRESS_FORMAT "\n",
                __func__,
                hdr->protocol,
                IP_ADDRESS_ARGS(hdr->src_address));
            break;
    }

    ExitPacketHandler();
}

shared_ptr NetH_IPv4ReassemblePacket(vnode* nic, unassembled_ip_packet* packet)
{
    OBOS_UNUSED(nic);
    ip_fragment* fragment = LIST_GET_HEAD(ip_fragments, &packet->fragments);
    shared_ptr ptr = {};
    OBOS_SharedPtrConstructSz(&ptr, Allocate(OBOS_KernelAllocator, packet->size, nullptr), packet->size);
    OBOS_SharedPtrRef(&ptr);
    while (fragment)
    {
        ip_fragment* next = LIST_GET_NEXT(ip_fragments, &packet->fragments, fragment);
        memcpy((char*)ptr.obj+fragment->offset, 
               (void*)((uintptr_t)fragment->hdr + IPv4_GET_HEADER_LENGTH(fragment->hdr)), 
               be16_to_host(fragment->hdr->packet_length));
        LIST_REMOVE(ip_fragments, &packet->fragments, fragment);
        OBOS_SharedPtrUnref(fragment->hdr_ptr);
        Free(OBOS_KernelAllocator, fragment, sizeof(*fragment));
        fragment = next;
    }
    return ptr;
}

RB_GENERATE(unassembled_ip_packets, unassembled_ip_packet, node, ip_packet_cmp);
LIST_GENERATE(ip_fragments, ip_fragment, node);

obos_status NetH_SendIPv4PacketMac(vnode *nic, void *ent_ /* ip_table_entry */, ip_addr dest, const mac_address dest_mac, uint8_t protocol, uint8_t ttl, uint8_t service_type, shared_ptr *data)
{
    if (!nic || !ent_ || !data)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ip_table_entry* ent = ent_;
    size_t sz = data->szObj + sizeof(ip_header) + sizeof(ethernet2_header);
    char* pckt = Allocate(OBOS_KernelAllocator, sz, nullptr);

    ip_header* hdr = (void*)(pckt+sizeof(ethernet2_header));
    memzero(hdr, sizeof(*hdr)); 
    hdr->dest_address = dest;
    hdr->src_address = ent->address;
    hdr->packet_length = host_to_be16(data->szObj+sizeof(ip_header));
    hdr->protocol = protocol;
    hdr->version_hdrlen = 0x45;
    hdr->id = 0;
    hdr->flags_fragment = host_to_be16(IPv4_DONT_FRAGMENT);
    hdr->service_type = host_to_be16(service_type);
    hdr->time_to_live = ttl;
    hdr->chksum = host_to_be16(NetH_OnesComplementSum(hdr, sizeof(*hdr)));
    memcpy(hdr+1, data->obj, data->szObj);
    OBOS_SharedPtrUnref(data); 

    ethernet2_header* eth_hdr = (void*)pckt;
    memcpy(eth_hdr->src, nic->net_tables->mac, sizeof(mac_address));
    memcpy(eth_hdr->dest, dest_mac, sizeof(mac_address));
    eth_hdr->type = host_to_be16(ETHERNET2_TYPE_IPv4);

    shared_ptr *data_ptr = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(shared_ptr), nullptr);
    OBOS_SharedPtrConstructSz(data_ptr, pckt, sz);
    data_ptr->free = OBOS_SharedPtrDefaultFree;
    data_ptr->freeUdata = OBOS_KernelAllocator;
    data_ptr->onDeref = NetFreeSharedPtr;

    return NetH_SendEthernetPacket(nic, OBOS_SharedPtrCopy(data_ptr));
}

obos_status NetH_SendIPv4Packet(vnode *nic, void *ent_, ip_addr dest, uint8_t protocol, uint8_t ttl, uint8_t service_type, shared_ptr *data)
{
    mac_address dest_mac = {};
    obos_status status = NetH_ResolveExternalIP(nic, dest, &dest_mac);
    if (obos_is_error(status))
        return status;
    
    return NetH_SendIPv4PacketMac(nic, ent_, dest, dest_mac, protocol, ttl, service_type, data);
}
