/*
 * oboskrnl/net/frame.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <utils/list.h>

#include <irq/dpc.h>

#include <vfs/vnode.h>

typedef struct net_shared_buffer {
    size_t refcount;
    void* base;
    size_t buff_size;
} net_shared_buffer;
void NetH_ReleaseSharedBuffer(net_shared_buffer* b);

typedef LIST_HEAD(frame_queue, struct frame) frame_queue;
typedef struct frame {
    uint8_t* buff;
    size_t sz;

    uint32_t source_ip;
    uint16_t source_port;

    net_shared_buffer* base;
    // The MAC address of the NIC that received this frame.
    uint8_t interface_mac_address[6];
    // The vnode of the NIC that received this frame
    vnode* interface_vn;
    dpc receive_dpc;
    LIST_NODE(frame_queue, struct frame) node;
} frame;
LIST_PROTOTYPE(frame_queue, frame, node);
