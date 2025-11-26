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

#include <irq/timer.h>

#include <vfs/socket.h>

#include <utils/tree.h>
#include <utils/list.h>

#ifndef TCP_MAX_RETRANSMISSIONS
#   define TCP_MAX_RETRANSMISSIONS 10
#endif

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

    bool check_tx_window : 1;

    // Terminated by an "end of option list" option
    // Is to be copied directly into the tcp header.
    struct tcp_option* options;
    size_t option_list_size; // in bytes, not entries

    uint8_t ttl;

    shared_ptr* payload;
    size_t payload_offset;
    size_t payload_size;

    uint64_t expiration_ms;

    struct tcp_unacked_segment* unacked_seg;
};

enum {
    TCP_STATE_INVALID,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RECEIVED,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT1,
    TCP_STATE_FIN_WAIT2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_CLOSING,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT,
    TCP_STATE_CLOSED,
};

typedef struct tcp_unacked_segment {
    shared_ptr ptr;
    
    timer expiration_timer;
    bool expired;
    bool sent;
    
    LIST_NODE(tcp_unacked_segment_list, struct tcp_unacked_segment) node;
    
    struct tcp_pseudo_hdr segment;
    
    uint32_t nBytesUnACKed;
    uint32_t nBytesInFlight;
    event evnt;
    
    struct tcp_connection* con;
    
    uint8_t nRetries;
} tcp_unacked_segment;
typedef LIST_HEAD(tcp_unacked_segment_list, tcp_unacked_segment) tcp_unacked_segment_list;
LIST_PROTOTYPE(tcp_unacked_segment_list, tcp_unacked_segment, node);

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
    vnode* nic;

    struct {
        void* buf;
        size_t size;
        size_t ptr;
        size_t in_ptr;
        mutex lock;
        bool closed : 1;
    } recv_buffer;

    event inbound_sig;
    event inbound_urg_sig;

    bool write_closed : 1;

    bool got_icmp_msg : 1;

    shared_ptr *icmp_header_ptr;
    struct icmp_header* icmp_header;
    
    struct {
        struct {
            // oldest unacknowledged sequence number
            uint32_t una;
            // next sequence number to be sent
            uint32_t nxt;
            // send window
            uint32_t wnd;
            // send urgent pointer
            uint32_t up;
            // segment sequence number used for last window update
            uint32_t wl1;
            // segment acknowledgment number used for last window update
            uint32_t wl2;
            // initial send sequence number
            uint32_t iss;
        } snd;

        struct {
            // receive next
            uint32_t nxt;
            // receive window
            uint32_t wnd;
            // receive urgent pointer
            uint32_t up;
            // receive initial receive sequence number
            uint32_t irs;
        } rcv;

        int state;
        event state_change_event;
    } state;
    struct {
        tcp_unacked_segment_list list;
        pushlock lock;
    } unacked_segments;

    tcp_unacked_segment* fin_segment;

    uint8_t ttl;

    bool is_client : 1;
    bool accepted : 1;
    bool reset : 1;
    bool close_ack : 1;
    
    RB_ENTRY(tcp_connection) node;
} tcp_connection;

// Returns false if for some reason, the connection was reset by
// the function. This does *not* always mean that a RST was sent,
// but it does always mean that *something* was sent.
bool Net_TCPRemoteACKedSegment(tcp_connection* con, uint32_t ack);
void Net_TCPRetransmitSegment(tcp_unacked_segment* seg);
void Net_TCPCancelAllOutstandingSegments(tcp_connection* con);
void Net_TCPChangeConnectionState(tcp_connection* con, int state);
void Net_TCPPushReceivedData(tcp_connection* con, const void* buffer, size_t size, size_t *nPushed);
void Net_TCPPushDataToRemote(tcp_connection* con, const void* buffer, size_t size, bool oob);
void Net_TCPReset(tcp_connection* con);

static inline int tcp_connection_cmp(tcp_connection* lhs, tcp_connection* rhs)
{
    if (lhs->src.addr.addr < rhs->src.addr.addr) return -1;
    if (lhs->src.addr.addr > rhs->src.addr.addr) return 1;
    if (lhs->dest.port < rhs->dest.port) return -1;
    if (lhs->dest.port > rhs->dest.port) return 1;
    OBOS_ENSURE (lhs->is_client == rhs->is_client);
    if (lhs->is_client)
    {
        if (lhs->src.port < rhs->src.port) return -1;
        if (lhs->src.port > rhs->src.port) return 1;
    }
    return 0;
}
typedef RB_HEAD(tcp_connection_tree, tcp_connection) tcp_connection_tree;
RB_PROTOTYPE(tcp_connection_tree, tcp_connection, node, tcp_connection_cmp);

typedef struct tcp_port {
    uint16_t port;
    tcp_connection_tree connections;
    pushlock connection_tree_lock;
    event connection_event;
    struct net_tables* iface;
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

// con should be nullptr if you don't want to
// have this segment appended to the unseen
// segments list (e.g., if this is an ACK to
// another segment)
obos_status NetH_SendTCPSegment(vnode* nic, tcp_connection* con, void* ent /* ip_table_entry */, ip_addr dest, struct tcp_pseudo_hdr* dat);

PacketProcessSignature(TCP, ip_header*);

extern socket_ops Net_TCPSocketBackend;