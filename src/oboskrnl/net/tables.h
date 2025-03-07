/*
 * oboskrnl/net/tables.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vfs/vnode.h>

#include <net/ip.h>
#include <net/eth.h>
#include <net/udp.h>

#include <locks/rw_lock.h>
#include <locks/event.h>

#include <utils/list.h>
#include <utils/tree.h>

typedef struct ip_table_entry {
    ip_addr address;
    ip_addr broadcast_address;
    uint8_t subnet_mask;
    udp_queue_tree received_udp_packets;
    rw_lock received_udp_packets_tree_lock;
    bool icmp_echo_replies; // Whether to allow ICMP echo replies for this IP.
    LIST_NODE(ip_table, struct ip_table_entry) node;
} ip_table_entry;
typedef LIST_HEAD(ip_table, ip_table_entry) ip_table;
LIST_PROTOTYPE(ip_table, ip_table_entry, node);

typedef struct address_table_entry {
    ip_addr addr;
    mac_address phys;
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

/*
 * NOTE(oberrow): This comment is for my future reference. I might keep it around for anyone else to refer
 * to it, if they wish to do so.
 * When a program wishes to send a IPv4 packet to an IP address, it checks
 * tables.table.
foreach entry in tables.table:
    if ((address & BITS(0, entry.subnet_mask)) == entry.address)
        target_entry = entry
        break
    continue
 * If target_entry is non-null, the kernel routes the IPv4 packet via target_entry.address,
 * setting ipv4_header.source to target_entry.address, and destination to whatever our final destination should be.
 * The destination MAC address is set according to tables.address_to_phys, and in the case that there
 * is no entry in that table, we must use ARP to find the MAC address, then cache the IP address -> MAC address in
 * tables.address_to_phys.
 * Note that we should set a timeout for how long we can wait for an ARP reply to come, and return
 * destination unreachable if the timeout expires.
 * TODO: Would it be a good idea to retry the ARP request, or do we let userspace deal with that?
 *
 * If target_entry is nullptr, this means the address we want to send a packet to is on a different network.
 * In such case, we must route the packet through the gateway specified by gateway_phys.
 * We set ipv4_header.source to gateway_entry.address in the IPv4 packet, but we keep ipv4_header.destination to whatever we want our
 * final destination to be.
 * Note that the destination mac address should be gateway_entry.phys
 *
 * In the case that we have no gateway configured, either because the user never did so, or the user did so, but the
 * configuration was invalid, the kernel should return an error status saying the destination is unreachable if
 * target_entry is nullptr.
 *
 * Reception of packets needs not be changed, since as I said before,
 * ipv4_header.destination is not changed when someone sends a packet through a gateway.
 *
 * In the case that we receive an IPv4 packet with destination not matching anything in tables.table,
 * we must forward the packet to it's destination.
 * We do this as we would any other packet, but we need to pay attention to time_to_live.
 * Linux unconditionally decrements ttl, disregarding the actual time it took to process the packet.
 * (probably because it should never take more than one second to do this?)
 * If we end up discarding the packet because TTL is zero, we need to send an ICMP reply to the source
 * telling it that we timed out.
 */

// Set interface->data to this.
typedef struct tables {
    rw_lock table_lock;
    ip_table table;
    
    rw_lock address_to_phys_lock;
    address_table address_to_phys;

    address_table_entry* gateway_phys;
    ip_table_entry* gateway_entry;
    ip_addr gateway_address;
    event gateway_lock; // EVENT_SYNC
    
    vnode *interface;
    mac_address interface_mac;

    bool ipv4_forward;

    enum {
        IP_TABLES_MAGIC = 0x6b83764e04e022ed
    } magic;

    dev_desc desc;
} tables;