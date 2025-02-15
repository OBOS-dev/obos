/*
 * oboskrnl/net/tcp.h
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * Abandon all hope, ye who enter here.
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <struct_packing.h>
#include <memmanip.h>

#include <net/ip.h>
#include <net/tcp.h>

#include <allocators/base.h>

uint16_t NetH_TCPChecksum(const ip_header* ip_hdr, const tcp_header* tcp_hdr)
{
    struct {
        uint32_t src, dest;
        uint8_t zero;
        uint8_t ptcl;
        uint16_t tcp_len;
    } OBOS_PACK pseudo_header = {
        .dest = ip_hdr->dest_address.addr,
        .src = ip_hdr->src_address.addr,
        .ptcl = ip_hdr->protocol,
        .tcp_len = ip_hdr->packet_length-IPv4_GET_HEADER_LENGTH(ip_hdr)
    };

    const size_t sz = pseudo_header.tcp_len + sizeof(pseudo_header);
    uint8_t *data = OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, sz, nullptr);
    memcpy(data, &pseudo_header, sizeof(pseudo_header));
    memcpy(data+sizeof(pseudo_header), tcp_hdr, pseudo_header.tcp_len);
    
    uint16_t res = NetH_OnesComplementSum(data, sz);
    
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, data, sz);

    return res;
}

obos_status Net_FormatTCPPacket(tcp_header** phdr, const void* data, uint16_t length, uint16_t src_port, uint16_t dest_port, uint16_t window, uint16_t seq, uint16_t ack, uint16_t urg_ptr)
{
    if (!phdr || (!data && length) || !src_port || !dest_port)
        return OBOS_STATUS_INVALID_ARGUMENT;
    tcp_header *hdr = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(tcp_header)+length, nullptr);
    hdr->window = host_to_be16(window);
    hdr->ack_num = host_to_be16(ack);
    hdr->seq_num = host_to_be16(seq);
    hdr->urg_ptr = host_to_be16(urg_ptr);
    hdr->source = host_to_be16(src_port);
    hdr->dest = host_to_be16(dest_port);
    TCP_SET_HEADER_SIZE(hdr, sizeof(tcp_header));
    return OBOS_STATUS_SUCCESS;
}

obos_status Net_TCPReceiveFrame(const frame* what, const ip_header* ip_hdr, void *ent)
{
    tcp_header* hdr = (void*)what->buff;
    uint16_t hdr_checksum = hdr->chksum;
    hdr->chksum = 0;
    uint16_t our_checksum = NetH_TCPChecksum(ip_hdr, hdr);
    hdr->chksum = hdr_checksum;
    hdr_checksum = be16_to_host(hdr_checksum);

    if (hdr_checksum != our_checksum)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received TCP packet has invalid checksum (got 0x%04x, expected 0x%04x). Dropping packet.\n", 
            what->interface_mac_address[0], what->interface_mac_address[1], what->interface_mac_address[2], 
            what->interface_mac_address[3], what->interface_mac_address[4], what->interface_mac_address[5],
            hdr_checksum, our_checksum
        );
        return OBOS_STATUS_INVALID_HEADER;
    }

    return OBOS_STATUS_SUCCESS;
}