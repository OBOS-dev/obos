/*
 * oboskrnl/net/ip.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <net/ip.h>

#include <allocators/base.h>

uint16_t NetH_OnesComplementSum(void *buffer, size_t size)
{
    uint16_t *p = buffer;
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

uint16_t Net_IPChecksum(ip_header* hdr)
{
    return NetH_OnesComplementSum(hdr, sizeof(*hdr));
}

obos_status Net_FormatIPv4Packet(ip_header** phdr, void* data, uint16_t sz, uint8_t precedence, const ip_addr* restrict source, const ip_addr* restrict destination, uint8_t lifetime_seconds, uint8_t protocol, uint8_t service_type, bool override_preferred_size)
{
    if (!phdr || !data || !sz || !source || !destination)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!override_preferred_size && sz > IPv4_PREFERRED_PACKET_LENGTH)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ip_header* hdr = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(ip_header)+sz, nullptr);
    hdr->protocol = protocol;
    hdr->id_flags_fragment = 0;
    hdr->time_to_live = lifetime_seconds;
    hdr->service_type = (precedence & 0x3) | (service_type & 0x70);
    hdr->version_hdrlen = (4 << 4) | (sizeof(*hdr) / 4);
    hdr->packet_length = host_to_be16(sz+sizeof(ip_header));
    hdr->src_address.addr = source->addr;
    hdr->dest_address.addr = destination->addr;
    hdr->chksum = host_to_be16(Net_IPChecksum(hdr));
    memcpy(hdr+1, data, sz);
    *phdr = hdr;
    return OBOS_STATUS_SUCCESS;
}

obos_status Net_IPReceiveFrame(frame* data)
{
    OBOS_UNUSED(data);
    return OBOS_STATUS_SUCCESS;
}

LIST_GENERATE(frame_queue, frame, node);
