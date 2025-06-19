/*
 * oboskrnl/net/arp.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <vfs/vnode.h>

#include <locks/mutex.h>
#include <locks/event.h>

#include <net/eth.h>
#include <net/arp.h>
#include <net/tables.h>
#include <net/ip.h>
#include <net/macros.h>

#include <allocators/base.h>

#include <locks/pushlock.h>

#include <utils/tree.h>
#include <utils/list.h>
#include <utils/shared_ptr.h>

struct arp_header_payload
{
    mac_address sender_mac;
    ip_addr sender_ip;
    mac_address target_mac; // set to zero if unknown
    ip_addr target_ip;
};

PacketProcessSignature(ARPReply, arp_header*)
{
    OBOS_UNUSED(depth && buf && size);
    struct arp_header_payload* data = (void*)(userdata+1);
    address_table_entry what = {.addr = data->sender_ip};
    Core_PushlockAcquire(&nic->net_tables->table_lock, true);
    address_table_entry* ent = RB_FIND(address_table, &nic->net_tables->arp_cache, &what);
    Core_PushlockRelease(&nic->net_tables->table_lock, true);
    if (!ent)
        ExitPacketHandler();
    if (ent->phys[0])
        ExitPacketHandler();
    memcpy(ent->phys, data->sender_mac, sizeof(mac_address));
    Core_EventSet(&ent->sync, true);
    ExitPacketHandler();
}

PacketProcessSignature(ARPRequest, arp_header*)
{
    OBOS_UNUSED(depth && ptr && size);
    struct arp_header_payload* data = (void*)(userdata+1);
    ip_addr* target = &data->target_ip;
    Core_PushlockAcquire(&nic->net_tables->table_lock, true);
    ip_table_entry* ent = LIST_GET_HEAD(ip_table, &nic->net_tables->table);
    for (; ent; ent = LIST_GET_NEXT(ip_table, &nic->net_tables->table, ent))
    {
        if (ent->address.addr == target->addr)
            break;
    }
    Core_PushlockRelease(&nic->net_tables->table_lock, true);
    if (!ent)
        ExitPacketHandler();
    size_t real_size = sizeof(arp_header)+sizeof(ip_addr)*2+sizeof(mac_address)*2;
    arp_header* hdr = Allocate(OBOS_KernelAllocator, real_size, nullptr);
    struct arp_header_payload* payload = (void*)(hdr+1);
    payload->sender_ip = *target;
    memcpy(payload->sender_mac, nic->net_tables->mac, sizeof(mac_address));
    payload->target_ip = data->sender_ip;
    memcpy(payload->target_mac, data->sender_mac, sizeof(mac_address));
    hdr->hw_address_space = host_to_be16(ARP_HW_ADDRESS_SPACE_ETHERNET);
    hdr->protocol_address_space = host_to_be16(ETHERNET2_TYPE_IPv4);
    hdr->len_hw_address = sizeof(mac_address);
    hdr->len_protocol_address = sizeof(ip_addr);
    hdr->opcode = host_to_be16(ARP_REPLY);
    shared_ptr* packet = NetH_FormatEthernetPacket(nic, payload->target_mac, hdr, real_size, ETHERNET2_TYPE_ARP);
    Free(OBOS_KernelAllocator, hdr, real_size);
    NetH_SendEthernetPacket(nic, OBOS_SharedPtrCopy(packet));
    // We don't have ownership of 'packet'
    // OBOS_SharedPtrUnref(packet);
    ExitPacketHandler();
}

PacketProcessSignature(ARP, ethernet2_header*)
{
    arp_header* hdr = ptr;
    if (hdr->len_hw_address != 6)
        ExitPacketHandler();
    
    if (hdr->len_protocol_address != 4)
    {
        if (hdr->len_protocol_address == 16)
            NetUnimplemented(hdr->len_protocol_address == 16 (IPv6));
        ExitPacketHandler();
    }

    switch (be16_to_host(hdr->opcode))
    {
        case ARP_REPLY:
            InvokePacketHandler(ARPReply, ptr, size, hdr);
            break;
        case ARP_REQUEST:
            InvokePacketHandler(ARPRequest, ptr, size, hdr);
            break;
        default:
            NetError("%s: Unrecognized ARP opcode 0x%04x from " MAC_ADDRESS_FORMAT "\n",
                __func__,
                hdr->opcode,
                MAC_ADDRESS_ARGS(userdata->src));
            break;
    }
    ExitPacketHandler();
}