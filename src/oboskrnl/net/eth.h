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

obos_status Net_FormatEthernet2Packet(ethernet2_header** hdr, void* data, size_t sz, const mac_address* restrict dest, const mac_address* restrict src, uint16_t type, size_t *out_sz);
obos_status Net_EthernetUp(vnode* interface_vn);

// Each ethernet driver should define this
// argp points to a `mac_address`
#define IOCTL_ETHERNET_INTERFACE_MAC_REQUEST 0x1
// Each ethernet driver should define this
// argp should be converted to bool
// Return OBOS_STATUS_SUCCESS if the interface supports IP checksum offload, and it was set to argp.
// Return any error code otherwise.
#define IOCTL_ETHERNET_SET_IP_CHECKSUM_OFFLOAD 0x2
// Each ethernet driver should define this
// argp should be converted to bool
// Return OBOS_STATUS_SUCCESS if the interface supports UDP checksum offload, and it was set to argp.
// Return any error code otherwise.
#define IOCTL_ETHERNET_SET_UDP_CHECKSUM_OFFLOAD 0x3
// Each ethernet driver should define this
// argp should be converted to bool
// Return OBOS_STATUS_SUCCESS if the interface supports TCP checksum offload, and it was set to argp.
// Return any error code otherwise.
#define IOCTL_ETHERNET_SET_TCP_CHECKSUM_OFFLOAD 0x4
