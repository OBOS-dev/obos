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

#include <locks/mutex.h>
#include <locks/event.h>

#include <net/eth.h>
#include <net/ip.h>
#include <net/macros.h>

#include <utils/tree.h>
#include <utils/shared_ptr.h>

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

PacketProcessSignature(ARP, ethernet2_header*);