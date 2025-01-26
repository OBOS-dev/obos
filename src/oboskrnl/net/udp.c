/*
 * oboskrnl/net/udp.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <net/udp.h>
#include <net/ip.h>
#include <net/frame.h>

#include <allocators/base.h>

obos_status Net_FormatUDPPacket(udp_header** phdr, const void* data, uint16_t length, uint16_t src_port, uint16_t dest_port)
{
    if (!phdr || !data || !length || !src_port || !dest_port)
        return OBOS_STATUS_INVALID_ARGUMENT;
    udp_header* hdr = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(udp_header)+length, nullptr);
    hdr->length = host_to_be16(length+sizeof(udp_header));
    hdr->dest_port = host_to_be16(dest_port);
    hdr->src_port = host_to_be16(src_port);
    memcpy(hdr+1, data, length);
    *phdr = hdr;
    return OBOS_STATUS_SUCCESS;
}

obos_status Net_UDPReceiveFrame(frame* what, const frame* raw_frame)
{
    OBOS_UNUSED(raw_frame);
    udp_header* hdr = (void*)what->base;
    // TODO: Checksum validation
    if (be16_to_host(hdr->length) > what->sz)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received UDP packet has invalid packet size (NOTE: Buffer overflow). Dropping packet.\n", 
            what->source_mac_address[0], what->source_mac_address[1], what->source_mac_address[2], 
            what->source_mac_address[3], what->source_mac_address[4], what->source_mac_address[5]
        );
        return OBOS_STATUS_INVALID_HEADER;
    }
    NetH_ReleaseSharedBuffer(what->base);
    return OBOS_STATUS_SUCCESS;
}