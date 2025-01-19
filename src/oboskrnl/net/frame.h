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

typedef LIST_HEAD(frame_queue, struct frame) frame_queue;
typedef struct frame {
    uint8_t* buff;
    size_t sz;
    // The MAC address of the NIC that received this frame.
    uint8_t source_mac_address[6];
    // The vnode of the NIC that received this frame
    vnode* source_vn;
    dpc receive_dpc;
    LIST_NODE(frame_queue, struct frame) node;
} frame;
LIST_PROTOTYPE(frame_queue, frame, node);
