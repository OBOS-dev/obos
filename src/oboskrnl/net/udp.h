/*
 * oboskrnl/net/udp.h
 *
 * Copyright (c) 2025 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

#include <net/ip.h>
#include <net/macros.h>

#include <utils/list.h>
#include <utils/tree.h>
#include <utils/shared_ptr.h>

#include <locks/event.h>

#include <stdatomic.h>

typedef struct udp_header {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;
    uint16_t chksum;
} udp_header;

typedef struct udp_recv_packet {
    shared_ptr packet_ptr;
    shared_ptr buffer_ptr;
    struct {
        ip_addr addr;
        uint16_t port;
    } src;
    struct udp_port* bound_to;
    LIST_NODE(udp_port, struct udp_recv_packet) node;
} udp_recv_packet;
typedef LIST_HEAD(udp_recv_packet_list, struct udp_recv_packet) udp_recv_packet_list;
LIST_PROTOTYPE(udp_recv_packet_list, udp_recv_packet, node);
typedef struct udp_port {
    uint16_t port;
    udp_recv_packet_list packets;
    event recv_event;
    bool got_icmp_msg : 1;
    shared_ptr *icmp_header_ptr;
    struct icmp_header* icmp_header;
    RB_ENTRY(udp_port) node;
} udp_port;
typedef RB_HEAD(udp_port_tree, udp_port) udp_port_tree;
static inline int udp_port_cmp(const udp_port* lhs, const udp_port* rhs)
{
    if (lhs->port < rhs->port) return -1;
    if (lhs->port > rhs->port) return 1;
    return 0;
}
RB_PROTOTYPE(udp_port_tree, udp_port, node, udp_port_cmp);

PacketProcessSignature(UDP, ip_header*);

extern struct socket_ops Net_UDPSocketBackend;