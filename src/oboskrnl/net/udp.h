/*
 * oboskrnl/net/udp.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

typedef struct udp_header {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;
    uint16_t chksum;
} udp_header;

obos_status Net_FormatUDPPacket(udp_header** hdr, void* data, uint16_t length, uint16_t src_port, uint16_t dest_port);
