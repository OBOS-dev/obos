/*
 * oboskrnl/net/ip.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

typedef union ip_addr {
    uint32_t addr;
    struct {
        uint8_t comp1;
        uint8_t comp2;
        uint8_t comp3;
        uint8_t comp4;
    };
} OBOS_PACK ip_addr;

#define IPv4_GET_HEADER_LENGTH(hdr) ((be32_to_host((hdr)->version_hdrlen) & 0xf0) * 4)
#define IPv4_GET_HEADER_VERSION(hdr) (be32_to_host((hdr)->version_hdrlen) & 0x0f)

#define IPv4_GET_FLAGS(hdr) (be32_to_host((hdr)->flags_fragment) & 0x8)
#define IPv4_GET_FRAGMENT(hdr) (be32_to_host((hdr)->flags_fragment) & 0xfff8)

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
    IPv4_PREFERRED_PACKET_LENGTH = 576,
    IPv4_MAX_PACKET_LENGTH = 0xffff,
};

enum {
    IPv4_MAY_FRAGMENT = BIT(1),
    IPv4_LAST_FRAGMENT = BIT(2),
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
    uint16_t identification;
    uint16_t flags_fragment;

    // DW3
    uint8_t time_to_live;
    uint8_t protocol;
    uint16_t chksum;

    // DW4
    ip_addr src_address;
    
    // DW5
    ip_addr dest_address;

    // DW6
    // OPTIONAL!
    // Check IPv4_GET_HEADER_LENGTH(this) to make sure you can access this.
    uint32_t option;
} OBOS_PACK ip_header;