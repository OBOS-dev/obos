/*
 * init/nm.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <stdint.h>

void nm_initialize_interfaces(const char* config_file);

typedef union ip_addr {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    struct {
        uint8_t comp1;
        uint8_t comp2;
        uint8_t comp3;
        uint8_t comp4;
    };
#else
    struct {
        uint8_t comp4;
        uint8_t comp3;
        uint8_t comp2;
        uint8_t comp1;
    };
#endif
    uint32_t addr;
} ip_addr;
_Static_assert(sizeof(ip_addr) == 4, "Invalid ip_addr size");

typedef struct gateway_user {
    ip_addr src;
    ip_addr dest;
} gateway_user;

enum {
    IP_ENTRY_ENABLE_ICMP_ECHO_REPLY = 1<<0,
    IP_ENTRY_ENABLE_ARP_REPLY = 1<<1,
    IP_ENTRY_IPv4_FORWARDING = 1<<2,
};

typedef struct ip_table_entry_user {
    ip_addr address;
    ip_addr broadcast;
    uint32_t subnet;
    uint32_t ip_entry_flags;
} ip_table_entry_user;

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