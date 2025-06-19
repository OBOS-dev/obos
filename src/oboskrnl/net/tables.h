/*
* oboskrnl/net/tables.h
*
* Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <driver_interface/header.h>

#include <vfs/vnode.h>

#include <scheduler/thread.h>

#include <net/eth.h>
#include <net/ip.h>

#include <locks/pushlock.h>

#include <utils/list.h>

typedef struct gateway {
    LIST_NODE(gateway_list, struct gateway) node;
    ip_addr src;
    ip_addr dest;
    struct address_table_entry *cache;
} gateway;
typedef LIST_HEAD(gateway_list, gateway) gateway_list;
LIST_PROTOTYPE(gateway_list, gateway, node);

enum {
    IP_ENTRY_ENABLE_ICMP_ECHO_REPLY = BIT(0),
    IP_ENTRY_ENABLE_ARP_REPLY = BIT(1),
    IP_ENTRY_IPv4_FORWARDING = BIT(2),
};

typedef struct ip_table_entry {
    LIST_NODE(ip_table, struct ip_table_entry) node;
    ip_addr address;
    ip_addr broadcast;
    uint32_t subnet;
    uint32_t ip_entry_flags;
} ip_table_entry;
typedef LIST_HEAD(ip_table, ip_table_entry) ip_table;
LIST_PROTOTYPE(ip_table, ip_table_entry, node);

typedef struct address_table_entry {
    ip_addr addr;
    mac_address phys;
    // Wait on object before using this cache entry.
    event sync; 
    RB_ENTRY(address_table_entry) node;
} address_table_entry;
inline static int cmp_address_table_entry(const address_table_entry* lhs, const address_table_entry* rhs)
{
    if (lhs->addr.addr < rhs->addr.addr)
        return -1;
    if (lhs->addr.addr > rhs->addr.addr)
        return 1;
    return 0;
}
typedef RB_HEAD(address_table, address_table_entry) address_table;
RB_PROTOTYPE(address_table, address_table_entry, node, cmp_address_table_entry);

typedef struct net_tables {
    ip_table table;
    pushlock table_lock;

    address_table arp_cache;
    pushlock arp_cache_lock;

    gateway_list gateways;
    gateway* default_gateway;

    unassembled_ip_packets fragmented_packets;
    pushlock fragmented_packets_lock;

    vnode* interface;
    mac_address mac;
    dev_desc desc;

    enum {
        IP_TABLES_MAGIC = 0x6b83764e04e022ed
    } magic;

    thread* dispatch_thread;
    bool kill_dispatch;
} net_tables;

obos_status Net_Initialize(vnode* nic);
obos_status NetH_SendEthernetPacket(vnode *nic, shared_ptr* data);