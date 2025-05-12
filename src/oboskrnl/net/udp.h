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

PacketProcessSignature(UDP, ip_header*);