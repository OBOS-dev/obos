/*
 * oboskrnl/net/arp.h
 *
 * Copyright (c) 2025 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

#include <vfs/vnode.h>

#include <locks/event.h>

#include <net/eth.h>
#include <net/ip.h>

#include <utils/tree.h>

enum {
    ARP_REQUEST = 1,
    ARP_REPLY = 2,
};

enum {
    ARP_HW_ADDRESS_SPACE_ETHERNET = 1,
};

typedef struct OBOS_PACK arp_header {
    uint16_t hw_address_space;
    uint16_t protocol_address_space; // of type ETHERNET2_TYPE_*
    uint8_t len_hw_address; // 6 for ethernet
    uint8_t len_protocol_address; // 4 for IPv4, 16 for IPv6
    uint16_t opcode;
    uint8_t data[];
    // char sender_hw_address[len_hw_address];
    // char sender_protocol_address[len_protocol_address];
    // char target_hw_address[len_hw_address]; // set to zero if unknown
    // char target_protocol_address[len_protocol_address];
} arp_header;

// Formats an ARP request for IPv4
// sender_ip - our IP address
// sender_mac - our MAC address
// target_ip - the IP address we want to know about
obos_status Net_FormatARPRequestIPv4(arp_header** hdr, mac_address sender_mac, ip_addr sender_ip, ip_addr target_ip);
// Formats an ARP reply for IPv4
// sender_mac - our MAC address
// sender_ip - our IP address
// target_ip - the IP address that asked for us
// target_mac - the MAC address that asked for us
obos_status Net_FormatARPReplyIPv4(arp_header** hdr, mac_address sender_mac, ip_addr sender_ip, mac_address target_mac, ip_addr target_ip);

typedef struct arp_ip_request {
    vnode *interface;

    ip_addr target;

    mac_address* response;
    event evnt; // EVENT_NOTIFICATION

    RB_ENTRY(arp_ip_request) rb_node;
} arp_ip_request;
typedef RB_HEAD(arp_ip_request_tree, arp_ip_request) arp_ip_request_tree;
static inline int cmp_arp_ip_request(arp_ip_request* lhs, arp_ip_request* rhs)
{
    if (lhs->interface < rhs->interface)
        return -1;
    if (lhs->interface > rhs->interface)
        return 1;
    if (lhs->target.addr < rhs->target.addr)
        return -1;
    if (lhs->target.addr > rhs->target.addr)
        return 1;
    return 0;
}
RB_PROTOTYPE(arp_ip_request_tree, arp_ip_request, rb_node, cmp_arp_ip_request);

extern arp_ip_request_tree Net_InFlightARPRequests;
obos_status Net_ARPReceiveFrame(frame* data);
