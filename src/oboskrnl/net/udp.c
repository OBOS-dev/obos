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
