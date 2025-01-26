/*
 * oboskrnl/net/udp.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

#include <net/frame.h>
#include <net/ip.h>

#include <utils/tree.h>

typedef struct udp_header {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;
    uint16_t chksum;
} udp_header;

typedef struct udp_queue {
    frame_queue queue;
    pushlock lock;
    struct process* owning_process;
    uint16_t source_port;
    RB_ENTRY(udp_queue) rb_node;
} udp_queue;
static inline int cmp_udp_queue(const udp_queue* lhs, const udp_queue* rhs)
{
    if (lhs->source_port < rhs->source_port)
        return -1;
    if (lhs->source_port > rhs->source_port)
        return 1;
    return 0;
}
RB_HEAD(udp_queue_tree, udp_queue);
RB_PROTOTYPE(udp_queue_tree, udp_queue, rb_node, cmp_udp_queue);

obos_status Net_FormatUDPPacket(udp_header** hdr, const void* data, uint16_t length, uint16_t src_port, uint16_t dest_port);
obos_status Net_UDPReceiveFrame(frame* what, const frame* raw_frame);