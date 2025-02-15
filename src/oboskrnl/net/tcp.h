/*
 * oboskrnl/net/tcp.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

#include <net/ip.h>

#define TCP_SET_HEADER_SIZE(x, size) do {\
    (x)->data_offset_resv_ctrl = host_to_be16((((size) >> 2) & 0xf) | be16_to_host((x)->data_offset_resv_ctrl));\
} while(0)
#define TCP_SET_CTRL_BIT(x, mask) do {\
    (x)->data_offset_resv_ctrl = host_to_be16(mask) | be16_to_host((x)->data_offset_resv_ctrl));\
} while(0)

#define TCP_GET_HEADER_SIZE(x) ((be16_to_host((x)->data_offset_resv_ctrl) & 0xf) << 2)
#define TCP_GET_CTRL_BIT(x, mask) (be16_to_host((x)->data_offset_resv_ctrl) & (mask)))

enum {
    TCP_CTRL_URG = BIT(0) << 10,
    TCP_CTRL_ACK = BIT(1) << 10,
    TCP_CTRL_PSH = BIT(2) << 10,
    TCP_CTRL_RST = BIT(3) << 10,
    TCP_CTRL_SYN = BIT(4) << 10,
    TCP_CTRL_FIN = BIT(5) << 10,
};

typedef struct tcp_header {
    uint16_t source;
    uint16_t dest;
    uint32_t seq_num, ack_num;
    uint16_t data_offset_resv_ctrl;
    uint16_t window;
    uint16_t chksum;
    uint16_t urg_ptr;
    uint8_t options[];
} OBOS_PACK tcp_header;

// Set control bits after calling this using TCP_SET_CTRL_BIT
// NOTE: After doing that, recompute the checksum using NetH_TCPCHecksum
obos_status Net_FormatTCPPacket(ip_header* ip_hdr, tcp_header** hdr, const void* data, uint16_t length, uint16_t src_port, uint16_t dest_port, uint16_t window, uint16_t seq, uint16_t ack, uint16_t urg_ptr);
uint16_t NetH_TCPChecksum(ip_header* ip_hdr, tcp_header* tcp_hdr);