/*
 * oboskrnl/net/icmp.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <struct_packing.h>
#include <memmanip.h>

#include <net/eth.h>
#include <net/frame.h>
#include <net/icmp.h>
#include <net/tables.h>
#include <net/ip.h>

#include <allocators/base.h>

obos_status Net_FormatICMPv4Packet(icmp_header** phdr, const void* data, size_t sz, uint8_t type, uint8_t code, uint32_t usr)
{
    if (!phdr || ((!!data) != (!!sz)))
        return OBOS_STATUS_INVALID_ARGUMENT;

    icmp_header* hdr = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(*hdr)+sz, nullptr);
    hdr->type = type;
    hdr->code = code;
    hdr->usr = usr;
    if (sz)
        memcpy(hdr+1, data, sz);
    hdr->chksum = host_to_be16(NetH_OnesComplementSum(hdr, sizeof(*hdr)+sz));
    *phdr = hdr;

    return OBOS_STATUS_SUCCESS;
}

// ent points to struct ip_table_entry
obos_status Net_ICMPv4ReceiveFrame(const frame* what, const frame* raw_frame, void *pent)
{
    OBOS_UNUSED(raw_frame);
    ip_table_entry* ent = pent;
    OBOS_UNUSED(ent);
    const icmp_header* hdr = (void*)what->buff;
    const ip_header* ip_hdr = (void*)raw_frame->buff;
    const ethernet2_header* recv_eth_hdr = raw_frame->base->base;
    
    switch (hdr->type) {
        case ICMPv4_TYPE_ECHO_MSG:
        {
            if (!ent->icmp_echo_replies)
                break;

            // printf("sup\n");
            icmp_header* new_hdr = nullptr;
            size_t sz = what->sz-sizeof(*hdr);
            size_t hdr_sz = sz+sizeof(*hdr);
            ip_header* dest_ip_hdr = nullptr;
            ethernet2_header* eth_hdr = nullptr;
            size_t frame_size = 0;

            obos_status status = Net_FormatICMPv4Packet(&new_hdr, hdr+1, sz, ICMPv4_TYPE_ECHO_REPLY_MSG, 0, hdr->usr);
            if (obos_is_error(status))
                goto echo_cleanup;
            
            status = Net_FormatIPv4Packet(&dest_ip_hdr, new_hdr, hdr_sz, IPv4_PRECEDENCE_ROUTINE, &ip_hdr->dest_address, &ip_hdr->src_address, 60, 0x1 /* ICMPv4 */, 0, true);
            if (obos_is_error(status))
                goto echo_cleanup;

            // We actually can't do this.
            // status = Net_TransmitIPv4Packet(tbl->interface, dest_ip_hdr);

            status = Net_FormatEthernet2Packet(&eth_hdr, dest_ip_hdr, be16_to_host(dest_ip_hdr->packet_length), &recv_eth_hdr->src, &what->interface_mac_address, ETHERNET2_TYPE_IPv4, &frame_size);
            if (obos_is_error(status))
                goto echo_cleanup;

            status = what->interface_vn->un.device->driver->header.ftable.write_sync(what->interface_vn->tables->desc, eth_hdr, frame_size, 0, nullptr);
            
            echo_cleanup:
            // printf("%d %p %p %p\n", status, dest_ip_hdr, new_hdr, eth_hdr);
            if (dest_ip_hdr)
                OBOS_KernelAllocator->Free(OBOS_KernelAllocator, dest_ip_hdr, be16_to_host(dest_ip_hdr->packet_length));
            if (new_hdr)
                OBOS_KernelAllocator->Free(OBOS_KernelAllocator, new_hdr, hdr_sz);
            if (eth_hdr)
                OBOS_KernelAllocator->Free(OBOS_KernelAllocator, eth_hdr, frame_size);

            break;
        }
        case ICMPv4_TYPE_DEST_UNREACHABLE:
        case ICMPv4_TYPE_ECHO_REPLY_MSG:
            // TODO: Implement.
        default:
            break;
    }
    return OBOS_STATUS_SUCCESS;
}

obos_status Net_ICMPv4DestUnreachable(tables* tbl, const ip_header* ip_hdr, const frame* raw_frame, dest_unreachable_ec ec)
{
    OBOS_UNUSED(ip_hdr);
    if (!tbl || !ip_hdr || !raw_frame)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    const icmp_header* hdr = nullptr;
    size_t sz = be16_to_host(ip_hdr->packet_length);
    size_t hdr_sz = sz+sizeof(*hdr);
    ip_header* dest_ip_hdr = nullptr;
    ethernet2_header* eth_hdr = nullptr;
    ethernet2_header* recv_eth_hdr = (ethernet2_header*)raw_frame->base->base;
    size_t frame_size = 0;
    
    obos_status status = Net_FormatICMPv4Packet((icmp_header**)&hdr, ip_hdr, be16_to_host(ip_hdr->packet_length), ICMPv4_TYPE_DEST_UNREACHABLE, ec, 0);
    if (obos_is_error(status))
        goto cleanup;
    
    status = Net_FormatIPv4Packet(&dest_ip_hdr, hdr, hdr_sz, IPv4_PRECEDENCE_ROUTINE, &ip_hdr->dest_address, &ip_hdr->src_address, 60, 0x1 /* ICMPv4 */, 0, true);
    if (obos_is_error(status))
        goto cleanup;

    // We actually can't do this.
    // status = Net_TransmitIPv4Packet(tbl->interface, dest_ip_hdr);

    status = Net_FormatEthernet2Packet(&eth_hdr, dest_ip_hdr, be16_to_host(dest_ip_hdr->packet_length), &recv_eth_hdr->src, &tbl->interface_mac, ETHERNET2_TYPE_IPv4, &frame_size);
    if (obos_is_error(status))
        goto cleanup;

    status = tbl->interface->un.device->driver->header.ftable.write_sync(tbl->desc, eth_hdr, frame_size, 0, nullptr);

    cleanup:
    if (dest_ip_hdr)
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, dest_ip_hdr, be16_to_host(dest_ip_hdr->packet_length));
    if (hdr)
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, (icmp_header*)hdr, hdr_sz);
    if (eth_hdr)
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, eth_hdr, frame_size);

    return status;
}