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

#include <allocators/base.h>

#include <utils/list.h>

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
    OBOS_UNUSED(nic && depth && buf && size && userdata);
    udp_header* hdr = ptr;
    ip_header* ip_hdr = userdata;
    if (hdr->chksum)
        NetUnimplemented(UDP Checksums);
    void* udp_pckt_data = hdr+1;
    size_t udp_pckt_sz = size-sizeof(udp_header);
    udp_port key = {.port=be16_to_host(hdr->dest_port)};
    Core_PushlockAcquire(&nic->net_tables->udp_ports_lock, false);
    udp_port* dest = RB_FIND(udp_port_tree, &nic->net_tables->udp_ports, &key);
    if (!dest)
    {
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
    OBOS_SharedPtrRef(&pckt->buffer_ptr);
    
    pckt->src.addr = ip_hdr->src_address;
    pckt->src.port = be16_to_host(hdr->src_port);

    pckt->bound_to = dest;
    
    LIST_APPEND(udp_recv_packet_list, &dest->packets, pckt);
    
    Core_EventSet(&dest->recv_event, false);
    
    Core_PushlockRelease(&nic->net_tables->udp_ports_lock, false);

    ExitPacketHandler();
}

LIST_GENERATE(udp_recv_packet_list, udp_recv_packet, node);
RB_GENERATE(udp_port_tree, udp_port, node, udp_port_cmp);