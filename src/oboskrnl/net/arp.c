/*
 * oboskrnl/net/arp.h
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <error.h>
#include <struct_packing.h>
#include <memmanip.h>

#include <allocators/base.h>

#include <locks/event.h>
#include <locks/mutex.h>

#include <net/eth.h>
#include <net/ip.h>
#include <net/arp.h>
#include <net/tables.h>

#include <utils/tree.h>

// Formats an ARP request for IPv4
// sender_ip - our IP address
// sender_mac - our MAC address
// target_ip - the IP address we want to know about
OBOS_NO_UBSAN obos_status Net_FormatARPRequestIPv4(arp_header** phdr, mac_address sender_mac, ip_addr sender_ip, ip_addr target_ip)
{
    if (!phdr)
        return OBOS_STATUS_INVALID_ARGUMENT;
    arp_header* hdr = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(arp_header)+4*2+6*2, nullptr);
    hdr->hw_address_space = host_to_be16(ARP_HW_ADDRESS_SPACE_ETHERNET);
    hdr->protocol_address_space = host_to_be16(ETHERNET2_TYPE_IPv4);
    hdr->len_protocol_address = 4;
    hdr->len_hw_address = 6;
    hdr->opcode = host_to_be16(ARP_REQUEST);
    uint8_t* iter = (void*)hdr->data;
    // We don't know the target.
    mac_address target_mac = {};
    memcpy(iter, sender_mac, sizeof(mac_address));
    iter += sizeof(mac_address);
    memcpy(iter, &sender_ip, sizeof(sender_ip));
    iter += sizeof(sender_ip);
    memcpy(iter, target_mac, sizeof(mac_address));
    iter += sizeof(mac_address);
    memcpy(iter, &target_ip, sizeof(target_ip));
    iter += sizeof(target_ip);
    *phdr = hdr;
    return OBOS_STATUS_SUCCESS;
}

// Formats an ARP reply for IPv4
// sender_mac - our MAC address
// sender_ip - our IP address
// target_ip - the IP address that asked for us
// target_mac - the MAC address that asked for us
obos_status Net_FormatARPReplyIPv4(arp_header** phdr, mac_address sender_mac, ip_addr sender_ip, mac_address target_mac, ip_addr target_ip)
{
    if (!phdr)
        return OBOS_STATUS_INVALID_ARGUMENT;
    arp_header* hdr = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(arp_header)+4*2+6*2, nullptr);
    hdr->hw_address_space = host_to_be16(ARP_HW_ADDRESS_SPACE_ETHERNET);
    hdr->protocol_address_space = host_to_be16(ETHERNET2_TYPE_IPv4);
    hdr->len_protocol_address = 4;
    hdr->len_hw_address = 6;
    hdr->opcode = host_to_be16(ARP_REPLY);
    uint8_t* iter = (void*)hdr->data;
    memcpy(iter, sender_mac, sizeof(mac_address));
    iter += sizeof(mac_address);
    memcpy(iter, &sender_ip, sizeof(sender_ip));
    iter += sizeof(sender_ip);
    memcpy(iter, target_mac, sizeof(mac_address));
    iter += sizeof(mac_address);
    memcpy(iter, &target_ip, sizeof(target_ip));
    iter += sizeof(target_ip);
    *phdr = hdr;
    return OBOS_STATUS_SUCCESS;
}

RB_GENERATE(arp_ip_request_tree, arp_ip_request, rb_node, cmp_arp_ip_request);

arp_ip_request_tree Net_InFlightARPRequests;
mutex Net_InFlightARPRequestsLock;

obos_status Net_ARPReceiveFrame(frame* data)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    arp_header *hdr = (void*)data->buff;
    if (be16_to_host(hdr->hw_address_space) != ARP_HW_ADDRESS_SPACE_ETHERNET)
        goto exit;
    if (hdr->len_hw_address != 6)
        goto exit;
    if (hdr->len_protocol_address != 4)
        goto exit;
    if (be16_to_host(hdr->protocol_address_space) != ETHERNET2_TYPE_IPv4)
        goto exit;

    mac_address sender_mac = {};
    ip_addr sender_ip = {};
    mac_address target_mac = {};
    ip_addr target_ip = {};

    uint8_t* iter = (void*)hdr->data;
    memcpy(&sender_mac, iter, sizeof(mac_address));
    iter += sizeof(mac_address);
    memcpy(&sender_ip, iter, sizeof(sender_ip));
    iter += sizeof(sender_ip);
    memcpy(&target_mac, iter, sizeof(mac_address));
    iter += sizeof(mac_address);
    memcpy(&target_ip, iter, sizeof(target_ip));
    iter += sizeof(target_ip);

    switch (be16_to_host(hdr->opcode))
    {
        case ARP_REQUEST:
        {
            // OBOS_Debug("ARP: Request for %d.%d.%d.%d from %d.%d.%d.%d (%02x:%02x:%02x:%02x:%02x:%02x)\n",
            //            target_ip.comp1, target_ip.comp2, target_ip.comp3, target_ip.comp4,
            //            sender_ip.comp1, sender_ip.comp2, sender_ip.comp3, sender_ip.comp4,
            //            sender_mac[0], sender_mac[1], sender_mac[2],
            //            sender_mac[3], sender_mac[4], sender_mac[5]
            // );
            tables* tables = (void*)(uintptr_t)data->interface_vn->data;
            ip_table_entry *entry = nullptr;
            LIST_FOREACH(entry, ip_table, &tables->table)
            {
                if (entry->address.addr == target_ip.addr)
                {
                    // Setup the reply
                    arp_header* reply = nullptr;
                    status = Net_FormatARPReplyIPv4(&reply, data->interface_mac_address, entry->address, sender_mac, sender_ip);
                    if (obos_is_error(status))
                        break;

                    // Setup the frame
                    ethernet2_header* eth2 = nullptr;
                    size_t frame_size = 0;
                    status = Net_FormatEthernet2Packet(&eth2,
                                            reply, sizeof(*reply)+2*reply->len_protocol_address+2*reply->len_hw_address,
                                            (mac_address*)sender_mac, &data->interface_mac_address,
                                            ETHERNET2_TYPE_ARP,
                                            &frame_size);
                    if (obos_is_error(status))
                    {
                        OBOS_KernelAllocator->Free(OBOS_KernelAllocator,
                                                reply,
                                                sizeof(*reply)+2*reply->len_protocol_address+2*reply->len_hw_address);
                        break;
                    }

                    // Send the reply
                    status = data->interface_vn->un.device->driver->header.ftable.write_sync(data->interface_vn->tables->desc, eth2, frame_size, 0, nullptr);

                    // cleanup
                    OBOS_KernelAllocator->Free(OBOS_KernelAllocator,
                                                reply,
                                                sizeof(*reply)+2*reply->len_protocol_address+2*reply->len_hw_address);
                    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, eth2, frame_size);

                    // OBOS_Debug("ARP: We have %d.%d.%d.%d, replying to %d.%d.%d.%d\n",
                    //            target_ip.comp1, target_ip.comp2, target_ip.comp3, target_ip.comp4,
                    //            sender_ip.comp1, sender_ip.comp2, sender_ip.comp3, sender_ip.comp4
                    // );
                }
                break;
            }
            break;
        }
        case ARP_REPLY:
        {
            Core_MutexAcquire(&Net_InFlightARPRequestsLock);
            arp_ip_request what = { .target.addr=sender_ip.addr, .interface = data->interface_vn };
            arp_ip_request *request = RB_FIND(arp_ip_request_tree, &Net_InFlightARPRequests, &what);
            // OBOS_Debug("ARP: %02x:%02x:%02x:%02x:%02x:%02x has %d.%d.%d.%d\n",
            //     sender_mac[0], sender_mac[1], sender_mac[2],
            //     sender_mac[3], sender_mac[4], sender_mac[5],
            //     target_ip.comp1, target_ip.comp2, target_ip.comp3, target_ip.comp4
            // );
            if (!request)
            {
                Core_MutexRelease(&Net_InFlightARPRequestsLock);
                break;
            }
            memcpy(request->response, sender_mac, sizeof(mac_address));
            Core_EventSet(&request->evnt, true);
            RB_REMOVE(arp_ip_request_tree, &Net_InFlightARPRequests, request);
            Core_MutexRelease(&Net_InFlightARPRequestsLock);
            break;
        }
        default:
            status = OBOS_STATUS_UNIMPLEMENTED;
            break;
    }
    NetH_ReleaseSharedBuffer(data->base);
    exit:
    // OBOS_Debug("ARP: Finished handling packet\n");
    return status;
}
