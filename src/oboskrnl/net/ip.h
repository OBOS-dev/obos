/*
 * oboskrnl/net/ip.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

#include <vfs/vnode.h>

#include <net/macros.h>
#include <net/eth.h>

#include <utils/shared_ptr.h>

#include <utils/tree.h>
#include <utils/list.h>

typedef union ip_addr {
    struct {
        uint8_t comp1;
        uint8_t comp2;
        uint8_t comp3;
        uint8_t comp4;
    };
    uint32_t addr;
} OBOS_PACK ip_addr;
#define IP_ADDRESS_FORMAT "%d.%d.%d.%d"
#define IP_ADDRESS_ARGS(addr) addr.comp1,addr.comp2,addr.comp3,addr.comp4

#define IPv4_GET_HEADER_LENGTH(hdr) (((hdr)->version_hdrlen & 0x0f) * 4)
#define IPv4_GET_HEADER_VERSION(hdr) (((hdr)->version_hdrlen & 0xf0) >> 4)

#define IPv4_GET_FLAGS(hdr) ((be32_to_host((hdr)->id_flags_fragment) & 0xe000) >> 13)
#define IPv4_GET_FRAGMENT(hdr) (be32_to_host((hdr)->id_flags_fragment) & 0x1FFF)
#define IPv4_GET_ID(hdr) (be32_to_host((hdr)->id_flags_fragment) >> 16)

enum {
    IPv4_PRECEDENCE_ROUTINE,
    IPv4_PRECEDENCE_PRIORITY,
    IPv4_PRECEDENCE_IMMEDIATE,
    IPv4_PRECEDENCE_FLASH,
    IPv4_PRECEDENCE_FLASH_OVERRIDE,
    IPv4_PRECEDENCE_CRITICAL,
    IPv4_PRECEDENCE_INTERNETWORK_CONTROL,
    IPv4_PRECEDENCE_NETWORK_CONTROL,
};
enum {
    IPv4_DELAY_LOW = BIT(3),
    IPv4_HIGH_THROUGHPUT = BIT(4),
    IPv4_HIGH_RELIABLITY = BIT(5),
};

enum {
    // including the header
    IPv4_MAX_PACKET_LENGTH = 0xffff,
};

enum {
    IPv4_DONT_FRAGMENT = BIT(14),
    IPv4_MORE_FRAGMENTS = BIT(15),
};

// TODO: Are these valid?
enum {
    IPv4_OPTION_COPIED = BIT(0),
    IPv4_OPTION_CLASS_MASK = 0x6,
    IPv4_OPTION_NUMBER_MASK = 0xF8,
};

enum {
    IPv4_OPTION_CLASS_CONTROL,
    IPv4_OPTION_RESV1,
    IPv4_OPTION_DBG_MESUREMENT,
    IPv4_OPTION_RESV2,
};

// TODO: More stuff for "options"

// NOTE: Access all fields of this header through host_to_be* macros
typedef struct ip_header {
    // DW1
    uint8_t version_hdrlen;
    uint8_t service_type;
    /*
     * From RFC791 (IPv4 specification)
     * It is recommended that hosts only send datagrams
     * larger than 576 octets if they have assurance that the destination
     * is prepared to accept the larger datagrams.
    */
    uint16_t packet_length;
    
    // DW2
    uint32_t id_flags_fragment;

    // DW3
    uint8_t time_to_live; // in seconds
    uint8_t protocol;
    uint16_t chksum;

    // DW4
    ip_addr src_address;
    
    // DW5
    ip_addr dest_address;

    // After here, there can be options.
    // This is unimplemented.
} OBOS_PACK ip_header;

typedef struct ip_fragment {
    ip_header* hdr;
    shared_ptr* hdr_ptr;
    size_t offset;
    LIST_NODE(ip_fragments, struct ip_fragment) node;
} ip_fragment;
typedef LIST_HEAD(ip_fragments, struct ip_fragment) ip_fragments;
LIST_PROTOTYPE(ip_fragments, ip_fragment, node);

typedef struct unassembled_ip_packet
{
    struct net_tables *owner;
    shared_ptr This;
    ip_fragments fragments;
    union {
        uint64_t real_id;
        struct {
            ip_addr src;
            uint16_t id;
            uint16_t resv;
        };
    };
    size_t highest_offset;
    size_t size;
    RB_ENTRY(unassembled_ip_packet) node;
} unassembled_ip_packet;
inline static int ip_packet_cmp(unassembled_ip_packet* lhs, unassembled_ip_packet* rhs)
{
    if (lhs->real_id < rhs->real_id)
        return -1;
    if (lhs->real_id > rhs->real_id)
        return 1;
    return 0;
}
typedef RB_HEAD(unassembled_ip_packets, unassembled_ip_packet) unassembled_ip_packets;
RB_PROTOTYPE(unassembled_ip_packets, unassembled_ip_packet, node, ip_packet_cmp);
shared_ptr NetH_IPv4ReassemblePacket(vnode* nic, unassembled_ip_packet* packet);

PacketProcessSignature(IPv4, ethernet2_header*);