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
#include <net/udp.h>
#include <net/tcp.h>

#include <locks/pushlock.h>

#include <utils/list.h>
#include <utils/string.h>

typedef struct gateway {
    // The address the gateway handles
    ip_addr src; 
    // The gateway address
    ip_addr dest; 
    // The IP table entry that would be used to comunicate with dest
    struct ip_table_entry* dest_ent; 
    struct address_table_entry *cache;
    LIST_NODE(gateway_list, struct gateway) node;
} gateway;
typedef LIST_HEAD(gateway_list, gateway) gateway_list;
LIST_PROTOTYPE(gateway_list, gateway, node);

typedef struct gateway_user {
    ip_addr src;
    ip_addr dest;
} gateway_user;

enum {
    IP_ENTRY_ENABLE_ICMP_ECHO_REPLY = BIT(0),
    IP_ENTRY_ENABLE_ARP_REPLY = BIT(1),
    IP_ENTRY_IPv4_FORWARDING = BIT(2),
};

typedef struct ip_table_entry_user {
    ip_addr address;
    ip_addr broadcast;
    uint32_t subnet;
    uint32_t ip_entry_flags;
} ip_table_entry_user;

typedef struct ip_table_entry {
    ip_addr address;
    ip_addr broadcast;
    uint32_t subnet;
    uint32_t ip_entry_flags;
    LIST_NODE(ip_table, struct ip_table_entry) node;
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

struct route {
    ip_table_entry* ent;
    struct net_tables* iface;
    gateway* route;
    uint8_t ttl;
    uint8_t hops;
    ip_addr destination;
    LIST_NODE(route_list, struct route) node;
    RB_ENTRY(route) rb_node;
};
inline static int route_cmp(struct route* lhs, struct route* rhs)
{
    if (lhs->iface < rhs->iface) return -1;
    if (lhs->iface > rhs->iface) return 1;
    if (lhs->destination.addr < rhs->destination.addr) return -1;
    if (lhs->destination.addr > rhs->destination.addr) return 1;
    return 0;
}
typedef RB_HEAD(route_tree, route) route_tree;
RB_PROTOTYPE(route_tree, route, rb_node, route_cmp);

typedef LIST_HEAD(route_list, struct route) route_list;
LIST_PROTOTYPE(route_list, struct route, node);

typedef struct net_tables {
    ip_table table;
    pushlock table_lock;

    address_table arp_cache;
    pushlock arp_cache_lock;

    gateway_list gateways;
    gateway* default_gateway;

    unassembled_ip_packets fragmented_packets;
    pushlock fragmented_packets_lock;

    udp_port_tree udp_ports;
    pushlock udp_ports_lock;

    tcp_port_tree tcp_ports;
    pushlock tcp_ports_lock;

    route_tree cached_routes;
    pushlock cached_routes_lock;
    
    // Connections made by bind()ing
    // then connect()ing are put here;
    // tcp_port contains connections 
    // established by listen()ing on
    // a bound port.
    tcp_connection_tree tcp_outgoing_connections;
    pushlock tcp_connections_lock;

    vnode* interface;
    mac_address mac;
    dev_desc desc;

    enum {
        IP_TABLES_MAGIC = 0x6b83764e04e022ed
    } magic;

    thread* dispatch_thread;
    bool kill_dispatch;

    LIST_NODE(network_interface_list, struct net_tables) node;
} net_tables;
typedef LIST_HEAD(network_interface_list, net_tables) network_interface_list;
LIST_PROTOTYPE(network_interface_list, net_tables, node);
extern network_interface_list Net_Interfaces;

obos_status Net_Initialize(vnode* nic);
obos_status NetH_SendEthernetPacket(vnode *nic, shared_ptr* data);
obos_status NetH_AddressRoute(net_tables** interface, ip_table_entry** routing_entry, uint8_t *ttl, ip_addr destination);
obos_status NetH_GetLocalAddressInterface(net_tables** interface, ip_addr src);

OBOS_EXPORT obos_status Net_InterfaceIoctl(vnode* nic, uint32_t request, void* argp);
OBOS_EXPORT obos_status Net_InterfaceIoctlArgpSize(uint32_t request, size_t* argp_sz);

extern string Net_Hostname;

obos_status Sys_GetHostname(char* name, size_t len);
obos_status Sys_SetHostname(const char* name, size_t len);