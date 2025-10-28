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
        case ICMPv4_TYPE_TIME_EXCEEDED:
        case ICMPv4_TYPE_PARAMETER_PROBLEM:
        case ICMPv4_TYPE_DEST_UNREACHABLE:
        {
            ip_header* ip_hdr_2 = (void*)(hdr+1);
            switch (ip_hdr_2->protocol) {
                case 0x11 /* UDP */:
                {
                    udp_header* udp_hdr = (udp_header*)(ip_hdr_2 + 1);
                    udp_port key = {.port=be16_to_host(udp_hdr->src_port)};
                    Core_PushlockAcquire(&nic->net_tables->udp_ports_lock, true);
                    udp_port* bound = RB_FIND(udp_port_tree, &nic->net_tables->udp_ports, &key);
                    Core_PushlockRelease(&nic->net_tables->udp_ports_lock, true);
                    if (!bound)
                        break;
                    bound->got_icmp_msg = true;
                    bound->icmp_header = hdr;
                    if (bound->icmp_header_ptr)
                        OBOS_SharedPtrUnref(bound->icmp_header_ptr);
                    bound->icmp_header_ptr = OBOS_SharedPtrCopy(buf);
                    Core_EventSet(&bound->recv_event, false);
                    break;
                }
                case 0x6 /* TCP */:
                {
                    tcp_header* tcp_hdr = (tcp_header*)(ip_hdr_2 + 1);

                    tcp_port key = {.port=be16_to_host(tcp_hdr->dest_port)};
                    Core_PushlockAcquire(&nic->net_tables->tcp_ports_lock, true);
                    tcp_port* port = RB_FIND(tcp_port_tree, &nic->net_tables->tcp_ports, &key);
                    Core_PushlockRelease(&nic->net_tables->tcp_ports_lock, true);

                    tcp_connection conn_key = {
                        .dest= {
                            .addr=ip_hdr->src_address,
                            .port=be16_to_host(tcp_hdr->src_port),
                        },
                        .src= {
                            .addr=ip_hdr->dest_address,
                            .port=be16_to_host(tcp_hdr->dest_port),
                        },
                        .is_client = true,
                    };

                    Core_PushlockAcquire(&nic->net_tables->tcp_connections_lock, true);
                    tcp_connection* current_connection = RB_FIND(tcp_connection_tree, &nic->net_tables->tcp_outgoing_connections, &conn_key);
                    Core_PushlockRelease(&nic->net_tables->tcp_connections_lock, true);

                    if (port)
                    {
                        Core_PushlockAcquire(&port->connection_tree_lock, true);
                        current_connection = RB_FIND(tcp_connection_tree, &port->connections, &conn_key);
                        Core_PushlockRelease(&port->connection_tree_lock, true);
                    }

                    if (!current_connection)
                        break; // .....

                    current_connection->got_icmp_msg = true;
                    current_connection->icmp_header = hdr;
                    if (current_connection->icmp_header_ptr)
                        OBOS_SharedPtrUnref(current_connection->icmp_header_ptr);
                    current_connection->icmp_header_ptr = OBOS_SharedPtrCopy(buf);
                    Core_EventSet(&current_connection->inbound_sig, false);
                    break;
                }
                default: break;

            }
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

obos_status NetH_ICMPv4ResponseToStatus(icmp_header* hdr)
{
    obos_status status = OBOS_STATUS_INTERNAL_ERROR;
    switch (hdr->type) {
        case ICMPv4_TYPE_DEST_UNREACHABLE:
        {
            switch (hdr->code) {
                case ICMPv4_CODE_PORT_UNREACHABLE:
                case ICMPv4_CODE_PROTOCOL_UNREACHABLE:
                    status = OBOS_STATUS_CONNECTION_REFUSED;
                    break;
                case ICMPv4_CODE_NET_UNREACHABLE:
                case ICMPv4_CODE_HOST_UNREACHABLE:
                case ICMPv4_CODE_SOURCE_ROUTE_FAILED:
                    status = OBOS_STATUS_NO_ROUTE_TO_HOST;
                    break;
                default: status = OBOS_STATUS_INTERNAL_ERROR;
            }
            break;
        }
        case ICMPv4_TYPE_TIME_EXCEEDED:
            status = OBOS_STATUS_NO_ROUTE_TO_HOST;
            break;
        case ICMPv4_TYPE_PARAMETER_PROBLEM:
        default: status = OBOS_STATUS_INTERNAL_ERROR; break;
    }
    return status;
}