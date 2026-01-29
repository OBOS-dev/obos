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
#define MAC_ADDRESS_FORMAT "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC_ADDRESS_ARGS(addr) addr[0],addr[1],addr[2],addr[3],addr[4],addr[5]
#define MAC_BROADCAST (mac_address){0xff,0xff,0xff,0xff,0xff,0xff,}

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

enum {
    // Each ethernet driver should define this
    // argp points to a `mac_address`
    IOCTL_IFACE_MAC_REQUEST = 0xe100,
    // implementations of the following ioctls are in
    // tables.h 
    IOCTL_IFACE_ADD_IP_TABLE_ENTRY,
    IOCTL_IFACE_REMOVE_IP_TABLE_ENTRY,
    IOCTL_IFACE_ADD_ROUTING_TABLE_ENTRY,
    IOCTL_IFACE_REMOVE_ROUTING_TABLE_ENTRY,
    IOCTL_IFACE_SET_IP_TABLE_ENTRY,
    IOCTL_IFACE_CLEAR_ARP_CACHE,
    IOCTL_IFACE_CLEAR_ROUTE_CACHE,
    IOCTL_IFACE_GET_IP_TABLE,
    IOCTL_IFACE_GET_ROUTING_TABLE,
    IOCTL_IFACE_SET_DEFAULT_GATEWAY,
    IOCTL_IFACE_UNSET_DEFAULT_GATEWAY,
    IOCTL_IFACE_INITIALIZE,
};

OBOS_EXPORT uint32_t NetH_CRC32Bytes(const void* data, size_t sz);

OBOS_EXPORT PacketProcessSignature(Ethernet, void*);

shared_ptr* NetH_FormatEthernetPacket(vnode* nic, const mac_address dest, const void* data, size_t size, uint16_t type);