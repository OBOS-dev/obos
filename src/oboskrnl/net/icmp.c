/*
 * oboskrnl/net/icmp.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <net/ip.h>
#include <net/tables.h>
#include <net/macros.h>
#include <net/icmp.h>

#include <locks/pushlock.h>

#include <allocators/base.h>

#include <utils/list.h>
#include <utils/shared_ptr.h>

DefineNetFreeSharedPtr

// ent points to struct ip_table_entry
PacketProcessSignature(ICMPv4, ip_header*)
{
    OBOS_UNUSED(depth && size);
    ip_header* const ip_hdr = userdata;
    icmp_header* hdr = ptr;

    switch (hdr->type) {
        case ICMPv4_TYPE_ECHO_MSG:
        {
            Core_PushlockAcquire(&nic->net_tables->table_lock, true);
            ip_table_entry* ent = LIST_GET_HEAD(ip_table, &nic->net_tables->table);
            while (ent)
            {
                if (ent->address.addr == ip_hdr->dest_address.addr)
                    break;
                ent = LIST_GET_NEXT(ip_table, &nic->net_tables->table, ent);
            }
            Core_PushlockRelease(&nic->net_tables->table_lock, true);

            if (~ent->ip_entry_flags & IP_ENTRY_ENABLE_ICMP_ECHO_REPLY)
                break;
            
            size_t sz = be16_to_host(ip_hdr->packet_length) - IPv4_GET_HEADER_LENGTH(ip_hdr);
            icmp_header* reply = Allocate(OBOS_KernelAllocator, sz, nullptr);
            memcpy(reply, hdr, sz);
            reply->type = ICMPv4_TYPE_ECHO_REPLY_MSG;
            reply->chksum = 0;
            reply->chksum = be16_to_host(NetH_OnesComplementSum(reply, sz));

            shared_ptr *data = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(shared_ptr), nullptr);
            OBOS_SharedPtrConstructSz(data, reply, sz);
            data->free = OBOS_SharedPtrDefaultFree;
            data->freeUdata = OBOS_KernelAllocator;
            data->onDeref = NetFreeSharedPtr;

            NetH_SendIPv4PacketMac(nic, ent, ip_hdr->src_address, ((ethernet2_header*)buf->obj)->src, 0x1, 60, 0, OBOS_SharedPtrCopy(data));
            
            break;
        }
        default:
            break;
    }

    ExitPacketHandler();
}

#define ICMP_FUNC_BODY(_type, _code, _usr, _pckt_data, _src_mac) \
{\
    size_t _icmp_sz = sizeof(icmp_header)+8+IPv4_GET_HEADER_LENGTH(ip_hdr);\
    icmp_header* hdr = ZeroAllocate(OBOS_KernelAllocator, 1, _icmp_sz, nullptr);\
    memcpy(hdr+1, ip_hdr, IPv4_GET_HEADER_LENGTH(ip_hdr));\
    if (_pckt_data)\
        memcpy((void*)((uintptr_t)(hdr+1) + IPv4_GET_HEADER_LENGTH(ip_hdr)), _pckt_data, 8);\
    hdr->code = (_code);\
    hdr->type = (_type);\
    hdr->usr = be32_to_host(_usr);\
    hdr->chksum = be16_to_host(NetH_OnesComplementSum(hdr, _icmp_sz));\
\
    shared_ptr *data_ptr = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(shared_ptr), nullptr);\
    OBOS_SharedPtrConstructSz(data_ptr, hdr, _icmp_sz);\
    data_ptr->free = OBOS_SharedPtrDefaultFree;\
    data_ptr->freeUdata = OBOS_KernelAllocator;\
    data_ptr->onDeref = NetFreeSharedPtr;\
\
    Core_PushlockAcquire(&nic->net_tables->table_lock, true);\
    ip_table_entry* ent = LIST_GET_HEAD(ip_table, &nic->net_tables->table);\
    while (ent)\
    {\
        if (ent->address.addr == ip_hdr->dest_address.addr)\
            break;\
        ent = LIST_GET_NEXT(ip_table, &nic->net_tables->table, ent);\
    }\
    Core_PushlockRelease(&nic->net_tables->table_lock, true);\
\
    /* hdr is freed once the packet is sent */ \
    return NetH_SendIPv4PacketMac(nic, ent, ip_hdr->src_address, _src_mac, 0x1, 60, 0, OBOS_SharedPtrCopy(data_ptr));\
}

obos_status Net_ICMPv4DestUnreachable(vnode* nic, const ip_header* ip_hdr, const ethernet2_header* eth_hdr, void* pckt_data, dest_unreachable_ec code)
ICMP_FUNC_BODY(ICMPv4_TYPE_DEST_UNREACHABLE, code, 0, pckt_data, eth_hdr->src)

obos_status Net_ICMPv4TimeExceeded(vnode* nic, const ip_header* ip_hdr, const ethernet2_header* eth_hdr, void* pckt_data, time_exceeded_ec code)
ICMP_FUNC_BODY(ICMPv4_TYPE_TIME_EXCEEDED, code, 0, pckt_data, eth_hdr->src)

obos_status Net_ICMPv4ParameterProblem(vnode* nic, const ip_header* ip_hdr, const ethernet2_header* eth_hdr, void* pckt_data, uint8_t offset)
ICMP_FUNC_BODY(ICMPv4_TYPE_TIME_EXCEEDED, 0, offset, pckt_data, eth_hdr->src)
