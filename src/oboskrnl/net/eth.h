/*
 * oboskrnl/net/eth.h
 *
 * Copyright (c) 2025 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

#include <driver_interface/header.h>

#include <vfs/vnode.h>

#include <net/macros.h>

#include <utils/shared_ptr.h>

typedef uint8_t mac_address[6];
#define BROADCAST_MAC_ADDRESS {0xff,0xff,0xff,0xff,0xff,0xff,}

enum {
    ETHERNET2_TYPE_IPv4 = 0x0800,
    ETHERNET2_TYPE_ARP  = 0x0806,
    ETHERNET2_TYPE_IPv6 = 0x86dd,
};

typedef struct ethernet2_header {
    mac_address dest;
    mac_address src;
    uint16_t type;
} OBOS_PACK ethernet2_header;

// Each ethernet driver should define this
// argp points to a `mac_address`
#define IOCTL_ETHERNET_INTERFACE_MAC_REQUEST 0x1

PacketProcessSignature(Ethernet);