/*
 * oboskrnl/net/udp.h
 *
 * Copyright (c) 2025 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

#include <net/frame.h>
#include <net/ip.h>

#include <utils/tree.h>

#include <locks/rw_lock.h>
#include <locks/event.h>

#include <stdatomic.h>

typedef struct udp_header {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;
    uint16_t chksum;
} udp_header;

typedef struct udp_queue {
    frame_queue queue;
    struct process* owning_process;
    uint16_t dest_port;
    
    atomic_flag destroy;

    rw_lock lock;
    event recv_event; // EVENT_NOTIFICATION
    
    RB_ENTRY(udp_queue) rb_node;
} udp_queue;
static inline int cmp_udp_queue(const udp_queue* lhs, const udp_queue* rhs)
{
    if (lhs->dest_port < rhs->dest_port)
        return -1;
    if (lhs->dest_port > rhs->dest_port)
        return 1;
    return 0;
}
typedef RB_HEAD(udp_queue_tree, udp_queue) udp_queue_tree;
RB_PROTOTYPE(udp_queue_tree, udp_queue, rb_node, cmp_udp_queue);

obos_status Net_FormatUDPPacket(udp_header** hdr, const void* data, uint16_t length, uint16_t src_port, uint16_t dest_port);
// ent points to struct ip_table_entry
obos_status Net_UDPReceiveFrame(const frame* what, const frame* raw_frame, void *ent);
// ent points to struct ip_table_entry
udp_queue* NetH_GetUDPQueueForPort(void* ent, uint16_t port, bool create);
// ent points to struct ip_table_entry
void NetH_DestroyUDPQueue(void* ent, udp_queue* queue);