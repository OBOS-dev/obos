/*
 * oboskrnl/net/tcp.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <klog.h>
#include <struct_packing.h>

#include <net/macros.h>
#include <net/ip.h>

#include <locks/pushlock.h>
#include <locks/event.h>

#include <utils/tree.h>
#include <utils/list.h>

enum {
    TCP_FIN = BIT(0),
    TCP_SYN = BIT(1),
    TCP_RST = BIT(2),
    TCP_PSH = BIT(3),
    TCP_ACK = BIT(4),
    TCP_URG = BIT(5),
    TCP_ECE = BIT(6),
    TCP_CWR = BIT(7),
};

typedef struct tcp_header
{
    uint16_t src_port;
    uint16_t dest_port;
    
    uint32_t seq;
    
    uint32_t ack;

    // 4-bits, counted in dwords,
    // not bytes
    uint8_t data_offset;
    uint8_t flags;
    uint16_t window;

    uint16_t chksum;
    uint16_t urg_ptr;

    char data[];
} OBOS_PACK tcp_header;

enum {
    TCP_STATE_INVALID,
    TCP_STATE_SYN,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT1,
    TCP_STATE_FIN_WAIT2,
    TCP_STATE_CLOSED,
    TCP_STATE_TIME_WAIT,
};

// typedef struct tcp_incoming_packet {
//     void* buffer;
//     size_t size;
//     struct tcp_connection* con; 
//     LIST_NODE(tcp_incoming_list, struct tcp_incoming_packet) node;
// } tcp_incoming_packet;
// typedef LIST_HEAD(tcp_incoming_list, tcp_incoming_packet) tcp_incoming_list;
// LIST_PROTOTYPE(tcp_incoming_list, tcp_incoming_packet, node);
typedef struct tcp_connection {
    struct {
        ip_addr addr;
        uint16_t port;
    } src;
    
    struct {
        ip_addr addr;
        uint16_t port;
    } dest;

    struct ip_table_entry* ip_ent;

    // tcp_incoming_list incoming_packets;
    // pushlock incoming_packet_lock;
    // event incoming_event;

    struct {
        void* buf;
        size_t size;
        size_t ptr;
    } recv_buffer;
    event sig;
    // Remote "ACK" signal
    event ack_sig;
    
    int state;
    uint32_t last_seq;
    uint32_t last_ack;
    
    uint32_t rx_window;
    struct {
        int8_t shift;
        uint16_t window;
    } rx_window_shift;
    uint32_t tx_window;
    int8_t tx_window_shift;
    uint16_t remote_mss;

    uint8_t ttl;

    bool is_client;
    RB_ENTRY(tcp_connection) node;
} tcp_connection;
static inline int tcp_connection_cmp(tcp_connection* lhs, tcp_connection* rhs)
{
    if (lhs->src.addr.addr < rhs->src.addr.addr) return -1;
    if (lhs->src.addr.addr > rhs->src.addr.addr) return 1;
    if (lhs->src.port < rhs->src.port) return -1;
    if (lhs->src.port > rhs->src.port) return 1;
    OBOS_ENSURE (lhs->is_client == rhs->is_client);
    if (lhs->is_client)
    {
        if (lhs->dest.port < rhs->dest.port) return -1;
        if (lhs->dest.port > rhs->dest.port) return 1;
    }
    return 0;
}
typedef RB_HEAD(tcp_connection_tree, tcp_connection) tcp_connection_tree;
RB_PROTOTYPE(tcp_connection_tree, tcp_connection, node, tcp_connection_cmp);

typedef struct tcp_port {
    uint16_t port;
    tcp_connection_tree connections;
    pushlock connection_list_lock;
    event connection_event;
    RB_ENTRY(tcp_port) node;
} tcp_port;
static inline int tcp_port_cmp(tcp_port* lhs, tcp_port* rhs)
{
    if (lhs->port < rhs->port) return -1;
    if (lhs->port > rhs->port) return 1;
    return 0;
}
typedef RB_HEAD(tcp_port_tree, tcp_port) tcp_port_tree;
RB_PROTOTYPE(tcp_port_tree, tcp_port, node, tcp_port_cmp);

struct tcp_option {
    uint8_t kind;
    uint8_t len;
    uint8_t data[];
} OBOS_PACK;

struct tcp_pseudo_hdr {
    uint16_t src_port, dest_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t flags;
    uint16_t window;

    // Terminated by an "end of option list" option
    // Is to be copied directly into the tcp header.
    struct tcp_option* options;
    size_t option_list_size; // in bytes, not entries

    uint8_t ttl;

    shared_ptr* payload;
    size_t payload_offset;
    size_t payload_size;
};
obos_status NetH_SendTCPSegment(vnode* nic, void* ent /* ip_table_entry */, ip_addr dest, struct tcp_pseudo_hdr* dat);

PacketProcessSignature(TCP, ip_header*);

void NetH_TestTCP(vnode* nic, void* ent_);

obos_status NetH_TCPEstablishConnection(vnode* nic,
                                        void* ent /* ip_table_entry */, 
                                        ip_addr dest, 
                                        uint16_t src_port /* optional */, uint16_t dest_port,
                                        uint32_t rx_window_size /* input buffer size */,
                                        uint16_t mss,
                                        tcp_connection** con);
obos_status NetH_TCPTransmitPacket(vnode* nic, tcp_connection* con, shared_ptr* payload);
obos_status NetH_TCPCloseConnection(vnode* nic, tcp_connection* con);