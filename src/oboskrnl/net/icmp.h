/*
 * oboskrnl/net/icmp.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

#include <net/frame.h>
#include <net/ip.h>
#include <net/tables.h>

enum {
    ICMPv4_TYPE_ECHO_REPLY_MSG = 0,
    ICMPv4_TYPE_DEST_UNREACHABLE = 3,
    ICMPv4_TYPE_ECHO_MSG = 8,
    ICMPv4_TYPE_TIME_EXCEEDED = 11,
    ICMPv4_TYPE_PARAMETER_PROBLEM = 12,
};

// For ICMPv4_TYPE_DEST_UNREACHABLE
typedef enum {
    ICMPv4_CODE_NET_UNREACHABLE = 0,
    ICMPv4_CODE_HOST_UNREACHABLE,
    ICMPv4_CODE_PROTOCOL_UNREACHABLE,
    ICMPv4_CODE_PORT_UNREACHABLE,
    ICMPv4_CODE_FRAG_DF_SET,
    ICMPv4_CODE_SOURCE_ROUTE_FAILED,
} dest_unreachable_ec;
// For ICMPv4_TYPE_TIME_EXCEEDED
typedef enum {
    ICMPv4_CODE_TTL_EXCEEDED = 0,
    ICMPv4_CODE_FRAGMENT_REASSEMBLY_EXCEEDED = 1,
} time_exceeded_ec;

typedef struct icmp_header {
    uint8_t type;
    uint8_t code;
    // One's complement of the header+data
    uint16_t chksum;
    uint32_t usr;
    char data[];
} OBOS_PACK icmp_header;

// NOTE: 'usr' is added to the header as is! If it needs to be converted to network
// byte order, do so while passing it.
obos_status Net_FormatICMPv4Packet(icmp_header** hdr, const void* data, size_t sz, uint8_t type, uint8_t code, uint32_t usr);
// ent points to struct ip_table_entry
obos_status Net_ICMPv4ReceiveFrame(const frame* what, const frame* raw_frame, void *ent);

// Sends a Destination Unreachable ICMPv4 message to hdr->src_address.
obos_status Net_ICMPv4DestUnreachable(tables* tbl, const ip_header* hdr, const frame* raw_frame, dest_unreachable_ec code);
obos_status Net_ICMPv4TimeExceeded(tables* tbl, const ip_header* hdr, const frame* raw_frame, time_exceeded_ec code);
obos_status Net_ICMPv4ParameterProblem(tables* tbl, const ip_header* hdr, const frame* raw_frame, uint8_t offset);