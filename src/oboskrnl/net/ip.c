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
#include <net/udp.h>
#include <net/nic_data.h>

#include <allocators/base.h>

RB_GENERATE(address_table, address_table_entry, node, cmp_address_table_entry);
LIST_GENERATE(ip_table, ip_table_entry, node);

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
    return NetH_OnesComplementSum(hdr, IPv4_GET_HEADER_LENGTH(hdr));
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
    ip_header* hdr = (void*)data->buff;
    // verify the header's integrity.
    uint16_t tmp = hdr->chksum;
    hdr->chksum = 0;
    uint16_t chksum = Net_IPChecksum(hdr);
    hdr->chksum = tmp;
    if (be16_to_host(tmp) != chksum)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received IPv4 frame has invalid checksum. Expected 0x%04x, got 0x%04x. Dropping packet.\n", 
            data->source_mac_address[0], data->source_mac_address[1], data->source_mac_address[2], 
            data->source_mac_address[3], data->source_mac_address[4], data->source_mac_address[5],
            chksum, be16_to_host(tmp)    
        );
        return OBOS_STATUS_INVALID_HEADER;
    }
    if (IPv4_GET_HEADER_VERSION(hdr) != 4)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received IPv4 frame has unsupported version IPv%d. Dropping packet.\n", 
            data->source_mac_address[0], data->source_mac_address[1], data->source_mac_address[2], 
            data->source_mac_address[3], data->source_mac_address[4], data->source_mac_address[5],
            IPv4_GET_HEADER_VERSION(hdr)
        );
        return OBOS_STATUS_UNIMPLEMENTED;
    }
    if (IPv4_GET_HEADER_LENGTH(hdr) < 5)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received IPv4 frame has header size less than 20. Dropping packet.\n", 
            data->source_mac_address[0], data->source_mac_address[1], data->source_mac_address[2], 
            data->source_mac_address[3], data->source_mac_address[4], data->source_mac_address[5]);
        return OBOS_STATUS_INVALID_HEADER;
    }
    if (be16_to_host(hdr->packet_length) > data->sz)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received IPv4 frame has invalid size (NOTE: Buffer overflow). Dropping packet.\n", 
            data->source_mac_address[0], data->source_mac_address[1], data->source_mac_address[2], 
            data->source_mac_address[3], data->source_mac_address[4], data->source_mac_address[5]
        );
        return OBOS_STATUS_INVALID_HEADER;
    }
    if (IPv4_GET_HEADER_LENGTH(hdr) > data->sz)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received IPv4 frame has invalid header size (NOTE: Buffer overflow). Dropping packet.\n", 
            data->source_mac_address[0], data->source_mac_address[1], data->source_mac_address[2], 
            data->source_mac_address[3], data->source_mac_address[4], data->source_mac_address[5]
        );
        return OBOS_STATUS_INVALID_HEADER;
    }
    if (IPv4_GET_HEADER_LENGTH(hdr) > be16_to_host(hdr->packet_length))
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received IPv4 frame has invalid header size (NOTE: Larger than header's packet length field). Dropping packet.\n", 
            data->source_mac_address[0], data->source_mac_address[1], data->source_mac_address[2], 
            data->source_mac_address[3], data->source_mac_address[4], data->source_mac_address[5]
        );
        return OBOS_STATUS_INVALID_HEADER;
    }
    frame what = *data;
    what.buff += IPv4_GET_HEADER_LENGTH(hdr);
    what.sz -= IPv4_GET_HEADER_LENGTH(hdr);
    what.base->refcount++;
    obos_status status = OBOS_STATUS_UNIMPLEMENTED;
    switch (hdr->protocol) {
        // UDP
        case 0x11:
            data->base->refcount++;
            status = Net_UDPReceiveFrame(&what, data);
            break;
        default: break;
    }
    NetH_ReleaseSharedBuffer(data->base);
    return status;
}

LIST_GENERATE(frame_queue, frame, node);
