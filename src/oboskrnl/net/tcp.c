/*
 * oboskrnl/net/tcp.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include "error.h"
#include "irq/irql.h"
#include <int.h>
#include <struct_packing.h>
#include <memmanip.h>

#include <net/macros.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <net/tables.h>
#include <net/icmp.h>

#include <scheduler/process.h>
#include <scheduler/thread_context_info.h>
#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <locks/pushlock.h>
#include <locks/event.h>

#include <irq/timer.h>

#include <utils/tree.h>
#include <utils/list.h>
#include <utils/shared_ptr.h>

#include <vfs/alloc.h>
#include <vfs/socket.h>

#include <mm/alloc.h>

#include <allocators/base.h>

DefineNetFreeSharedPtr

static uint16_t tcp_chksum(const void *seg1, size_t sz_seg1, const void* seg2, size_t sz_seg2)
{
    const uint16_t *p = seg1;
    size_t size = sz_seg1;
    int sum = 0;
    
    for (int i = 0; i < ((int)size & ~(1)); i += 2) {
        sum += be16_to_host(p[i >> 1]);
    }

    p = seg2;
    size = sz_seg2;
    
    for (int i = 0; i < ((int)size & ~(1)); i += 2) {
        sum += be16_to_host(p[i >> 1]);
    }

    if (size & 1) {
        sum += be16_to_host((uint16_t)((uint8_t *)p)[size-1]);
    }

    sum = (sum >> 16) + (sum & 0xffff);
    sum += sum >> 16;

    uint16_t ret = ~sum;
    return ret;
}

static void tcp_seg_expired(void* userdata)
{
    struct tcp_unacked_segment* seg = userdata;
    seg->expired = true;
    OBOS_SharedPtrUnref(&seg->ptr);
}

obos_status NetH_SendTCPSegment(vnode* nic, tcp_connection* con, void* ent_ /* ip_table_entry */, ip_addr dest, struct tcp_pseudo_hdr* dat)
{
    ip_table_entry *ent = ent_;
    if (!nic || !ent || !dat)
        return OBOS_STATUS_INVALID_ARGUMENT;
    shared_ptr* payload = dat->payload;
    size_t payload_offset = dat->payload_offset;
    size_t payload_size = dat->payload_size;
    if (!dat->ttl)
        dat->ttl = 64;

    shared_ptr* ptr = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(shared_ptr), nullptr);
    size_t sz = sizeof(tcp_header) + (payload ? payload->szObj : 0) + dat->option_list_size;
    OBOS_SharedPtrConstructSz(ptr, Allocate(OBOS_KernelAllocator, sz, nullptr), sz);
    ptr->free = OBOS_SharedPtrDefaultFree;
    ptr->freeUdata = OBOS_KernelAllocator;
    ptr->onDeref = NetFreeSharedPtr;
    
    tcp_header* hdr = ptr->obj;
    memzero(hdr, sizeof(*hdr));
    hdr->window = be16_to_host(dat->window);
    hdr->flags = dat->flags;
    hdr->ack = be32_to_host(dat->ack);
    hdr->seq = be32_to_host(dat->seq);
    hdr->dest_port = be16_to_host(dat->dest_port);
    hdr->src_port = be16_to_host(dat->src_port);
    hdr->urg_ptr = 0;
    hdr->data_offset = ((sizeof(*hdr)+dat->option_list_size)/4) << 4;
    struct {
        uint32_t src_addr;
        uint32_t dest_addr;
        uint8_t zero;
        uint8_t protocol;
        uint16_t tcp_length;
    } OBOS_PACK ip_psuedo_header = {.src_addr=ent->address.addr,.dest_addr=dest.addr,.protocol=0x6,.zero=0,.tcp_length=host_to_be16(sz)};
    if (dat->options)
        memcpy(hdr->data, dat->options, dat->option_list_size);
    if (payload)
    {
        memcpy(hdr->data + dat->option_list_size, payload->obj + payload_offset, OBOS_MIN(payload_size, payload->szObj));
        if (!con)
            OBOS_SharedPtrUnref(payload);
    }
    hdr->chksum = tcp_chksum(&ip_psuedo_header, sizeof(ip_psuedo_header), hdr, sz);
    hdr->chksum = be16_to_host(hdr->chksum);
    
    //printf("tcp tx segment (%d->%d): hdr->flags=0x%x, hdr->ack=0x%x, hdr->seq=0x%x\n", be16_to_host(hdr->src_port), be16_to_host(hdr->dest_port), hdr->flags, be32_to_host(hdr->ack), be32_to_host(hdr->seq));

    // NOTE: Keep the list locked until we append the unacked segment
    // since if we get an ACK immediately (this includes if we get preempted!),
    // and the TCP handler sees no unACKed segment, then this segment will be
    // spuriously retransmitted, which is probably bad.
    
    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    if (con)
        Core_PushlockAcquire(&con->unacked_segments.lock, false);    

    obos_status status = OBOS_STATUS_SUCCESS;
    bool defer_send = false;
    if (dat->check_tx_window && con)
    {
        uint32_t window_edge = con->state.snd.una + con->state.snd.wnd;
        defer_send = dat->seq > window_edge;
        if (!defer_send)
            con->state.snd.nxt = dat->seq + dat->payload_size;
    }
    
    if (!defer_send)
        status = NetH_SendIPv4Packet(nic, ent, dest, 0x6, dat->ttl, 0, OBOS_SharedPtrCopy(ptr));
    
    if (obos_is_error(status))
    {
        Core_LowerIrql(oldIrql);
        if (con)
            Core_PushlockRelease(&con->unacked_segments.lock, false);
        return status;
    }

    if (!con)
    {
        Core_LowerIrql(oldIrql);
        return OBOS_STATUS_SUCCESS;
    }
    
    tcp_unacked_segment* seg = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(tcp_unacked_segment), nullptr);
    OBOS_SharedPtrConstructSz(&seg->ptr, seg, sizeof(*seg));
    seg->ptr.free = OBOS_SharedPtrDefaultFree;
    seg->ptr.freeUdata = OBOS_KernelAllocator;
    seg->con = con;
    seg->expired = false;
    seg->sent = !defer_send;
    seg->nBytesUnACKed = dat->payload_size + !dat->payload_size;
    seg->nBytesInFlight = dat->payload_size + !dat->payload_size;
    seg->evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    
    seg->segment = *dat;
    seg->segment.options = Allocate(OBOS_KernelAllocator, dat->option_list_size, nullptr);
    memcpy(seg->segment.options, dat->options, dat->option_list_size);
    if(!seg->segment.expiration_ms)
        seg->segment.expiration_ms = 5*1000 /* 5 second default */;
    
    if (seg->sent)
    {
        OBOS_SharedPtrRef(&seg->ptr);
        seg->expiration_timer.userdata = seg;
        seg->expiration_timer.handler = tcp_seg_expired;
        Core_TimerObjectInitialize(&seg->expiration_timer, TIMER_MODE_DEADLINE, seg->segment.expiration_ms * 1000); 
    }

    OBOS_SharedPtrRef(&seg->ptr);
    if (!LIST_GET_NODE_COUNT(tcp_unacked_segment_list, &con->unacked_segments.list))
        con->state.snd.una = dat->seq;
    LIST_APPEND(tcp_unacked_segment_list, &con->unacked_segments.list, seg);

    OBOS_SharedPtrRef(&seg->ptr);
    dat->unacked_seg = seg;

    Core_PushlockRelease(&con->unacked_segments.lock, false);

    if (con->state.state == TCP_STATE_FIN_WAIT1 && dat->flags & TCP_FIN)
        con->fin_segment = seg;

    Core_LowerIrql(oldIrql);

    return OBOS_STATUS_SUCCESS;
}

uintptr_t mt_random(void);

void tx_tm_hnd(void* udata)
{
    event* evnt = udata;
    Core_EventSet(evnt, false);
}

LIST_GENERATE(tcp_unacked_segment_list, tcp_unacked_segment, node);

static void update_send_window(tcp_connection* con, tcp_header* hdr)
{
    if ((con->state.snd.una < be32_to_host(hdr->ack) && be32_to_host(hdr->ack) <= con->state.snd.nxt) || !con->state.snd.wl1)
    { 
        // Update the send window
        if (con->state.snd.wl1 < be32_to_host(hdr->seq) || (con->state.snd.wl1 == be32_to_host(hdr->seq) && con->state.snd.wl2 <= be32_to_host(hdr->ack)))
        {
            con->state.snd.wnd = be16_to_host(hdr->window);
            con->state.snd.wl1 = be16_to_host(hdr->seq);
            con->state.snd.wl2 = be16_to_host(hdr->ack);
        }
    }
}

PacketProcessSignature(TCP, ip_header*)
{
    ip_header* ip_hdr = userdata;
    OBOS_UNUSED(nic && depth && ip_hdr && buf && ptr && size);
    tcp_header* hdr = ptr;

    struct {
        uint32_t src_addr;
        uint32_t dest_addr;
        uint8_t zero;
        uint8_t protocol;
        uint16_t tcp_length;
    } OBOS_PACK ip_psuedo_header = {
        .src_addr=ip_hdr->src_address.addr,
        .dest_addr=ip_hdr->dest_address.addr,
        .protocol=0x6,
        .zero=0,
        .tcp_length=host_to_be16(be16_to_host(ip_hdr->packet_length)-IPv4_GET_HEADER_LENGTH(ip_hdr))};
    uint16_t remote_checksum = be16_to_host(hdr->chksum);
    hdr->chksum = 0;
    uint16_t local_checksum = tcp_chksum(&ip_psuedo_header, sizeof(ip_psuedo_header), hdr, be16_to_host(ip_psuedo_header.tcp_length));
    hdr->chksum = host_to_be16(remote_checksum);
    if (remote_checksum != local_checksum)
    {
        NetError("%s: Wrong TCP checksum in packet from " IP_ADDRESS_FORMAT ". Expected checksum is 0x%04x, remote checksum is 0x%04x\n",
            __func__,
            IP_ADDRESS_ARGS(ip_hdr->src_address),
            local_checksum,
            remote_checksum
        );
        ExitPacketHandler();
    }

    Core_PushlockAcquire(&nic->net_tables->table_lock, true);
    ip_table_entry* ent = LIST_GET_HEAD(ip_table, &nic->net_tables->table);
    while (ent)
    {
        if (ent->address.addr == ip_hdr->dest_address.addr)
            break;
        ent = LIST_GET_NEXT(ip_table, &nic->net_tables->table, ent);
    }
    Core_PushlockRelease(&nic->net_tables->table_lock, true);

    tcp_port key = {.port=be16_to_host(hdr->dest_port)};
    Core_PushlockAcquire(&nic->net_tables->tcp_ports_lock, true);
    tcp_port* port = RB_FIND(tcp_port_tree, &nic->net_tables->tcp_ports, &key);
    Core_PushlockRelease(&nic->net_tables->tcp_ports_lock, true);

    tcp_connection conn_key = {
        .dest= {
            .addr=ip_hdr->src_address,
            .port=be16_to_host(hdr->src_port),
        },
        .src= {
            .addr=ip_hdr->dest_address,
            .port=be16_to_host(hdr->dest_port),
        },
        .is_client = false,
    };

    tcp_connection* con = nullptr;
    
    if (port)
    {
        Core_PushlockAcquire(&port->connection_tree_lock, true);
        con = RB_FIND(tcp_connection_tree, &port->connections, &conn_key);
        Core_PushlockRelease(&port->connection_tree_lock, true);
    }
    if (!con)
    {
        Core_PushlockAcquire(&nic->net_tables->tcp_connections_lock, true);
        conn_key.is_client = true;
        con = RB_FIND(tcp_connection_tree, &nic->net_tables->tcp_outgoing_connections, &conn_key);
        Core_PushlockRelease(&nic->net_tables->tcp_connections_lock, true);
    }

    
    if ((!con || con->state.state == TCP_STATE_CLOSED) && !port)
    {
        // Net_ICMPv4DestUnreachable(nic, ip_hdr, (ethernet2_header*)buf->obj, hdr, ICMPv4_CODE_PORT_UNREACHABLE);
        struct tcp_pseudo_hdr resp = {};
        resp.flags = TCP_RST;
        resp.ttl = 64;
        resp.window = 0;
        if (hdr->flags & TCP_ACK)
            resp.seq = be32_to_host(hdr->ack);
        else
        {
            uint8_t header_length = (hdr->data_offset >> 4) * 4;
            uint32_t segment_length = be16_to_host(ip_hdr->packet_length)-IPv4_GET_HEADER_LENGTH(ip_hdr) - header_length;   
            resp.seq = 0;
            resp.ack = be32_to_host(hdr->seq)+segment_length+1;
            resp.flags |= TCP_ACK;
        }
        resp.src_port = be16_to_host(hdr->dest_port);
        resp.dest_port = be16_to_host(hdr->src_port);
        if (~hdr->flags & TCP_RST)
            NetH_SendTCPSegment(nic, nullptr, ent, ip_hdr->src_address, &resp); // Do not respond to a RST with a RST

        NetError("%s: TCP Port %d not bound to any socket.\n", __func__, key.port); 
        Core_PushlockRelease(&nic->net_tables->udp_ports_lock, false);
        ExitPacketHandler();
    }

    // The TCB is in LISTEN state, although we do not have a TCB yet
    if (!con)
    {
        if (hdr->flags & TCP_RST)
            ExitPacketHandler(); // ignoring RST
        if (hdr->flags & TCP_FIN)
            ExitPacketHandler(); // Ignoring FIN
        if (hdr->flags & TCP_ACK)
        {
            struct tcp_pseudo_hdr resp = {};
            resp.flags = TCP_RST;
            resp.ttl = 64;
            resp.seq = be32_to_host(hdr->ack);
            resp.src_port = be16_to_host(hdr->dest_port);
            resp.dest_port = be16_to_host(hdr->src_port);
            NetH_SendTCPSegment(nic, nullptr, ent, ip_hdr->src_address, &resp);
            ExitPacketHandler();
        }
        if (hdr->flags & TCP_SYN)
        {
            con = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(tcp_connection), nullptr);
            con->state.rcv.nxt = be32_to_host(hdr->seq)+1;
            con->state.rcv.irs = be32_to_host(hdr->seq);
            con->state.snd.iss = mt_random();
            con->state.snd.nxt = con->state.snd.iss + 1;
            con->state.snd.una = con->state.snd.iss;
            con->state.state_change_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
            con->state.state = TCP_STATE_SYN_RECEIVED;
            con->ttl = 64;
            con->dest.addr = ip_hdr->src_address;
            con->dest.port = be16_to_host(hdr->src_port);
            con->src.port = port->port;
            con->src.addr = ip_hdr->dest_address;
            con->is_client = false;
            con->ip_ent = ent;
            con->nic = nic;
            con->inbound_sig = EVENT_INITIALIZE(EVENT_NOTIFICATION);
            con->inbound_urg_sig = EVENT_INITIALIZE(EVENT_NOTIFICATION);
            con->recv_buffer.lock = MUTEX_INITIALIZE();
            con->unacked_segments.lock = PUSHLOCK_INITIALIZE();

            Core_PushlockAcquire(&port->connection_tree_lock, false);
            RB_INSERT(tcp_connection_tree, &port->connections, con);
            Core_EventSet(&port->connection_event, false);
            Core_PushlockRelease(&port->connection_tree_lock, false);
            
            struct tcp_pseudo_hdr resp = {};
            resp.seq = con->state.snd.iss;
            resp.ack = con->state.rcv.nxt;
            resp.dest_port = con->dest.port;
            resp.src_port = con->src.port;
            resp.flags = TCP_SYN|TCP_ACK;
            resp.window = con->state.rcv.wnd;
            NetH_SendTCPSegment(nic, con, ent, ip_hdr->src_address, &resp);
            OBOS_SharedPtrUnref(&resp.unacked_seg->ptr);
        }
        else
            ExitPacketHandler(); // Drop the segment. FIXME do we need to increase rx_dropped in the nic or not?
    }
    else if (con->state.state == TCP_STATE_SYN_SENT)
    {
        if (hdr->flags & TCP_FIN)
            ExitPacketHandler();
        if (hdr->flags & TCP_ACK)
        {
            if (be32_to_host(hdr->ack) <= con->state.snd.iss || be32_to_host(hdr->ack) > con->state.snd.nxt)
            {
                struct tcp_pseudo_hdr resp = {};
                resp.src_port = be16_to_host(hdr->dest_port);
                resp.dest_port = be16_to_host(hdr->src_port);
                resp.ttl = con->ttl;
                resp.seq = be32_to_host(hdr->ack);
                resp.flags = TCP_RST;
                if (~hdr->flags & TCP_RST)
                    NetH_SendTCPSegment(nic, nullptr, ent, con->dest.addr, &resp);
                ExitPacketHandler();
            }
            // The ACK is acceptable, carry on
        }
        if (hdr->flags & TCP_RST)
        {
            // The TCP spec demands that we only do this
            // if the ACK is acceptable, which we check
            // above
            con->reset = true;
            Net_TCPChangeConnectionState(con, TCP_STATE_CLOSED);
            ExitPacketHandler();
        }
        else if (hdr->flags & TCP_SYN)
        {
            if (hdr->flags & TCP_ACK)
                Net_TCPRemoteACKedSegment(con, be32_to_host(hdr->ack));
            
            con->state.rcv.irs = be32_to_host(hdr->seq);
            con->state.rcv.nxt = be32_to_host(hdr->seq)+1;

            if (con->state.snd.una > con->state.snd.iss)
            {
                struct tcp_pseudo_hdr resp = {};
                resp.src_port = be16_to_host(hdr->dest_port);
                resp.dest_port = be16_to_host(hdr->src_port);
                resp.ttl = con->ttl;
                resp.seq = con->state.snd.nxt;
                resp.window = con->state.rcv.wnd;
                resp.ack = con->state.rcv.nxt;
                resp.flags = TCP_ACK;
                NetH_SendTCPSegment(nic, nullptr, ent, con->dest.addr, &resp);
                Net_TCPChangeConnectionState(con, TCP_STATE_ESTABLISHED);
                update_send_window(con, hdr);
            }
            else
            {
                struct tcp_pseudo_hdr resp = {};
                resp.src_port = be16_to_host(hdr->dest_port);
                resp.dest_port = be16_to_host(hdr->src_port);
                resp.ttl = con->ttl;
                resp.seq = con->state.snd.iss;
                resp.window = con->state.rcv.wnd;
                resp.ack = con->state.rcv.nxt;
                resp.flags = TCP_ACK | TCP_SYN;
                NetH_SendTCPSegment(nic, con, ent, con->dest.addr, &resp);
                OBOS_SharedPtrUnref(&resp.unacked_seg->ptr);
                Net_TCPChangeConnectionState(con, TCP_STATE_SYN_RECEIVED);
            }
        }
        else
            ExitPacketHandler();
    }
    else
    {
        // Check acceptability
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        uint8_t header_length = (hdr->data_offset >> 4) * 4;
#else
        uint8_t header_length = (hdr->data_offset & 0xf) * 4;
#endif
        uint32_t segment_length = be16_to_host(ip_hdr->packet_length)-IPv4_GET_HEADER_LENGTH(ip_hdr) - header_length;   
        void* segment_data = (char*)hdr + header_length;
        
        bool acceptable = false;

        if (!segment_length && !con->state.rcv.wnd)
            acceptable = be32_to_host(hdr->seq) == con->state.rcv.nxt;
        else if (!segment_length && con->state.rcv.wnd > 0)
            acceptable = con->state.rcv.nxt <= be32_to_host(hdr->seq) && 
                         be32_to_host(hdr->seq) < (con->state.rcv.nxt+con->state.rcv.wnd);
        else if (segment_length > 0 && !con->state.rcv.wnd)
            acceptable = false;
        else if (segment_length > 0 && con->state.rcv.wnd > 0)
            acceptable = (con->state.rcv.nxt <= be32_to_host(hdr->seq) && 
                         be32_to_host(hdr->seq) < (con->state.rcv.nxt+con->state.rcv.wnd)) ||
                         (con->state.rcv.nxt <= (be32_to_host(hdr->seq)+segment_length-1) && 
                         (be32_to_host(hdr->seq)+segment_length-1) < (con->state.rcv.nxt+con->state.rcv.wnd));
        else 
            OBOS_UNREACHABLE;

        if (!acceptable)
        {
            // This is UNACCEPTABLE!
            // (pun intended)
            //printf("received unacceptable segment! discarding\n");
            //printf("NOTE: PCKT.SEQ=%d, PCKT.ACK=%d, RCV.NXT=%d, RCV.WND=%d, PCKT.LEN=%d\n", 
            //     be32_to_host(hdr->seq)-con->state.rcv.irs, be32_to_host(hdr->ack)-con->state.snd.iss,
            //     con->state.rcv.nxt, con->state.rcv.wnd, segment_length
            // );
            if (hdr->flags & TCP_RST)
                ExitPacketHandler();
            struct tcp_pseudo_hdr resp = {};
            resp.src_port = be16_to_host(hdr->dest_port);
            resp.dest_port = be16_to_host(hdr->src_port);
            resp.ttl = con->ttl;
            resp.seq = con->state.snd.nxt;
            resp.ack = con->state.rcv.nxt;
            resp.window = con->state.rcv.wnd;
            resp.flags = TCP_ACK;
            NetH_SendTCPSegment(nic, nullptr, ent, con->dest.addr, &resp);
            ExitPacketHandler();
        }

        // TODO(oberrow): Allow processing of out-of-order segments.
        if (be32_to_host(hdr->seq) > con->state.rcv.nxt)
            ExitPacketHandler();

        if (hdr->flags & TCP_RST)
        {
            switch (con->state.state) {
                case TCP_STATE_SYN_RECEIVED:
                {
                    if (con->is_client)
                    {
                        con->reset = true;
                        Net_TCPChangeConnectionState(con, TCP_STATE_CLOSED);
                    }
                    else
                        Free(OBOS_KernelAllocator, con, sizeof(*con));
                    ExitPacketHandler();
                }
                case TCP_STATE_ESTABLISHED:
                case TCP_STATE_FIN_WAIT1:
                case TCP_STATE_FIN_WAIT2:
                case TCP_STATE_CLOSE_WAIT:
                {
                    con->reset = true;
                    Net_TCPChangeConnectionState(con, TCP_STATE_CLOSED);
                    Net_TCPCancelAllOutstandingSegments(con);
                    ExitPacketHandler();
                }
                case TCP_STATE_CLOSING:
                case TCP_STATE_LAST_ACK:
                case TCP_STATE_TIME_WAIT:
                {
                    Net_TCPChangeConnectionState(con, TCP_STATE_CLOSED);
                    ExitPacketHandler();
                }
                default:
                    ExitPacketHandler();
            }
        }
        if (hdr->flags & TCP_SYN)
        {
            con->reset = true;
            Net_TCPChangeConnectionState(con, TCP_STATE_CLOSED);
            Net_TCPCancelAllOutstandingSegments(con);
            struct tcp_pseudo_hdr resp = {};
            resp.src_port = be16_to_host(hdr->dest_port);
            resp.dest_port = be16_to_host(hdr->src_port);
            resp.ttl = con->ttl;
            resp.seq = be32_to_host(hdr->ack);
            resp.ack = 0;
            resp.flags = TCP_RST;
            NetH_SendTCPSegment(nic, con, ent, con->dest.addr, &resp);
            OBOS_SharedPtrUnref(&resp.unacked_seg->ptr);
            ExitPacketHandler();
        }
        if (hdr->flags & TCP_ACK)
        {
            switch (con->state.state) {
                case TCP_STATE_SYN_RECEIVED:
                {
                    Net_TCPChangeConnectionState(con, TCP_STATE_ESTABLISHED);
                    break;
                }
                case TCP_STATE_ESTABLISHED:
                case TCP_STATE_FIN_WAIT1:
                case TCP_STATE_FIN_WAIT2:
                case TCP_STATE_CLOSE_WAIT:
                case TCP_STATE_CLOSING:
                {
                    // Remote acknoledged our packets, probably.
                    if (!Net_TCPRemoteACKedSegment(con, be32_to_host(hdr->ack)))
                        ExitPacketHandler();
                    update_send_window(con, hdr);
                    if (con->state.state == TCP_STATE_FIN_WAIT1)
                    {
                        OBOS_ENSURE(con->fin_segment);
                        if (!con->fin_segment->nBytesUnACKed)
                        {
                            // Our FIN was ACKed, move into FIN-WAIT-2
                            Net_TCPChangeConnectionState(con, TCP_STATE_FIN_WAIT2);
                        }
                    }
                    else if (con->state.state == TCP_STATE_FIN_WAIT2)
                    {
                        Core_PushlockAcquire(&con->unacked_segments.lock, true);
                        if (!LIST_GET_NODE_COUNT(tcp_unacked_segment_list, &con->unacked_segments.list))
                        {
                            Core_PushlockRelease(&con->unacked_segments.lock, true);
                            con->close_ack = true;
                            Core_EventSet(&con->state.state_change_event, false);
                        }
                        else
                            Core_PushlockRelease(&con->unacked_segments.lock, true);
                    }
                    else if (con->state.state == TCP_STATE_CLOSING)
                    {
                        OBOS_ENSURE(con->fin_segment);
                        if (!con->fin_segment->nBytesUnACKed)
                        {
                            // Our FIN was ACKed, move into FIN-WAIT-2
                            Net_TCPChangeConnectionState(con, TCP_STATE_TIME_WAIT);
                        }
                        else
                            ExitPacketHandler();
                    }
                    break;
                }
                case TCP_STATE_LAST_ACK:
                {
                    Net_TCPChangeConnectionState(con, TCP_STATE_CLOSED);
                    break;
                }
                case TCP_STATE_TIME_WAIT:
                {
                    struct tcp_pseudo_hdr resp = {};
                    resp.src_port = be16_to_host(hdr->dest_port);
                    resp.dest_port = be16_to_host(hdr->src_port);
                    resp.ttl = con->ttl;
                    resp.seq = con->state.snd.nxt;
                    resp.window = con->state.rcv.wnd;
                    resp.ack = be32_to_host(hdr->seq);
                    resp.flags = TCP_ACK;
                    NetH_SendTCPSegment(nic, nullptr, ent, con->dest.addr, &resp);
                    break;
                }
                default: OBOS_UNREACHABLE;
            }
        }
        if (hdr->flags & TCP_URG)
        {
            con->state.rcv.up = OBOS_MAX(con->state.rcv.up, be32_to_host(hdr->urg_ptr));
            Core_EventSet(&con->inbound_urg_sig, false);
        }
        if (segment_length)
        {
            switch (con->state.state) {
                case TCP_STATE_ESTABLISHED:
                case TCP_STATE_FIN_WAIT1:
                case TCP_STATE_FIN_WAIT2:
                {
                    // Copy the data into our buffer, and ACK the data
                    size_t nPushed = 0;
                    Net_TCPPushReceivedData(con, segment_data, segment_length, &nPushed);
                    con->state.rcv.nxt += nPushed;
                    //printf("TCP window (%d, %d) receive window %d->%d\n", con->dest.port, con->src.port, con->state.rcv.wnd, con->state.rcv.wnd-nPushed);
                    //printf("TCP: Expecting SEQ=%d next (current SEQ=%d)\n", con->state.rcv.nxt-con->state.rcv.irs, be32_to_host(hdr->seq) - con->state.rcv.irs);
                    con->state.rcv.wnd -= nPushed;
                    struct tcp_pseudo_hdr resp = {};
                    resp.src_port = be16_to_host(hdr->dest_port);
                    resp.dest_port = be16_to_host(hdr->src_port);
                    resp.ttl = con->ttl;
                    resp.seq = con->state.snd.nxt;
                    resp.ack = con->state.rcv.nxt;
                    resp.window = con->state.rcv.wnd;
                    resp.flags = TCP_ACK;
                    NetH_SendTCPSegment(nic, nullptr, ent, con->dest.addr, &resp);
                    break;
                }
                case TCP_STATE_CLOSE_WAIT:
                case TCP_STATE_CLOSING:
                case TCP_STATE_LAST_ACK:
                case TCP_STATE_TIME_WAIT:
                    break;
                default: OBOS_UNREACHABLE;
            }
        }
        if (hdr->flags & TCP_FIN)
        {
            struct tcp_pseudo_hdr resp = {};
            resp.src_port = be16_to_host(hdr->dest_port);
            resp.dest_port = be16_to_host(hdr->src_port);
            resp.ttl = con->ttl;
            resp.seq = con->state.snd.nxt;
            resp.ack = ++con->state.rcv.nxt;
            resp.window = con->state.rcv.wnd;
            resp.flags = TCP_ACK;
            NetH_SendTCPSegment(nic, nullptr, ent, con->dest.addr, &resp);
            switch (con->state.state) {
                case TCP_STATE_SYN_RECEIVED:
                case TCP_STATE_ESTABLISHED:
                {
                    Net_TCPChangeConnectionState(con, TCP_STATE_CLOSE_WAIT);
                    con->recv_buffer.closed = true;
                    Core_EventSet(&con->inbound_sig, false);
                    Core_EventSet(&con->state.state_change_event, false);
                    break;
                }
                case TCP_STATE_FIN_WAIT2:
                {
                    Net_TCPChangeConnectionState(con, TCP_STATE_TIME_WAIT);
                    break;
                }
                case TCP_STATE_FIN_WAIT1:
                {
                    if (!con->fin_segment->nBytesUnACKed)
                    {
                        con->recv_buffer.closed = true;
                        Core_EventSet(&con->inbound_sig, false);
                        Core_EventSet(&con->state.state_change_event, false);
                        Net_TCPChangeConnectionState(con, TCP_STATE_TIME_WAIT);
                    }
                    else
                        Net_TCPChangeConnectionState(con, TCP_STATE_CLOSING);
                    break;
                }
                case TCP_STATE_CLOSING:
                case TCP_STATE_CLOSE_WAIT:
                case TCP_STATE_LAST_ACK:
                // TODO: Time-wait timeout?
                case TCP_STATE_TIME_WAIT:
                    break; // no-op
            }
        }
    }
    
    ExitPacketHandler();
}

void Net_TCPCancelAllOutstandingSegments(tcp_connection* con)
{
    tcp_unacked_segment *seg = LIST_GET_HEAD(tcp_unacked_segment_list, &con->unacked_segments.list);
    while (seg)
    {
        Core_CancelTimer(&seg->expiration_timer);
        Core_EventSet(&seg->evnt, false);
        seg->expired = true;

        seg = LIST_GET_NEXT(tcp_unacked_segment_list, &con->unacked_segments.list, seg);
    }
}

void Net_TCPPushReceivedData(tcp_connection* con, const void* buffer, size_t sz, size_t *nPushed)
{
    if (!con || !buffer)
        return;
    Core_MutexAcquire(&con->recv_buffer.lock);
    if ((sz+con->recv_buffer.ptr) >= con->recv_buffer.size)
        sz = con->recv_buffer.size - con->recv_buffer.ptr;
    void* out_ptr = (void*)((uintptr_t)con->recv_buffer.buf + con->recv_buffer.ptr);    
    memcpy(out_ptr, buffer, sz);
    con->recv_buffer.ptr += sz;
    Core_EventSet(&con->inbound_sig, false);
    if (nPushed)
        *nPushed = sz;
    Core_MutexRelease(&con->recv_buffer.lock);
    return;
}

// static const char* state_strs[] = {
//     "INVALID",
//     "LISTEN",
//     "SYN_SENT",
//     "SYN_RECEIVED",
//     "ESTABLISHED",
//     "FIN_WAIT1",
//     "FIN_WAIT2",
//     "CLOSE_WAIT",
//     "CLOSING",
//     "LAST_ACK",
//     "TIME_WAIT",
//     "CLOSED",
// };

void Net_TCPChangeConnectionState(tcp_connection* con, int state)
{
    if (state > TCP_STATE_CLOSED || state < TCP_STATE_INVALID)
        return;
    //printf("TCP: Changing from %s to %s\n", con->state.state[state_strs], state[state_strs]);
    con->state.state = state;
    Core_EventPulse(&con->state.state_change_event, false);
    if (state == TCP_STATE_ESTABLISHED)
    {
        if (!con->state.rcv.wnd)
            con->state.rcv.wnd = 0x10000;
        con->recv_buffer.size = con->state.rcv.wnd;
        con->recv_buffer.ptr = 0;
        con->recv_buffer.closed = false;
        con->recv_buffer.buf = Allocate(OBOS_KernelAllocator, con->recv_buffer.size, nullptr);
    }
    else if (state == TCP_STATE_TIME_WAIT)
    {
        con->close_ack = true;
        Core_EventSet(&con->state.state_change_event, false);
    }
    else if (state == TCP_STATE_FIN_WAIT2)
    {
        con->close_ack = true;
        Core_EventSet(&con->state.state_change_event, false);
    }
}

bool Net_TCPRemoteACKedSegment(tcp_connection* con, uint32_t ack)
{
    if (ack < con->state.snd.una)
        return true; // ACK to an old segment, ignore
    
    Core_PushlockAcquire(&con->unacked_segments.lock, true);
    
    uint32_t nBytesACKed = (ack - con->state.snd.una);
    
    tcp_unacked_segment* seg = LIST_GET_HEAD(tcp_unacked_segment_list, &con->unacked_segments.list);
    while (nBytesACKed != 0)
    {
        /*
         * "A segment on the retransmission queue is fully acknowledged if the sum
         * of its sequence number and length is less or equal than the
         * acknowledgment value in the incoming segment."
         * (RFC 793)
         */
        if ((seg->nBytesInFlight + seg->segment.seq) <= ack)
        {
            nBytesACKed -= seg->nBytesUnACKed;
            con->state.snd.una += seg->nBytesUnACKed;
            seg->nBytesUnACKed = 0;
            //printf("remote acked packet flags=0x%x remote acked 0x%p seg.seq=0x%p\n", seg->segment.flags, ack - con->state.snd.iss, seg->segment.seq - con->state.snd.iss);
        }
        else
        {
            seg->nBytesUnACKed -= nBytesACKed;
            con->state.snd.una += nBytesACKed;
            nBytesACKed = 0;
        }

        tcp_unacked_segment *next = LIST_GET_NEXT(tcp_unacked_segment_list, &con->unacked_segments.list, seg);
        
        if (!seg->nBytesUnACKed)
        {
            // Retake the lock as a writer, remove the segment,
            // and continue
            Core_PushlockRelease(&con->unacked_segments.lock, true);
            Core_PushlockAcquire(&con->unacked_segments.lock, false);
            
            Core_EventSet(&seg->evnt, false);
            LIST_REMOVE(tcp_unacked_segment_list, &con->unacked_segments.list, seg);
            Core_CancelTimer(&seg->expiration_timer);
            OBOS_SharedPtrUnref(&seg->ptr);

            Core_PushlockRelease(&con->unacked_segments.lock, false);
            Core_PushlockAcquire(&con->unacked_segments.lock, true);
        }
        
        seg = next;
        // Even if the remote has acknoledged more bytes, we don't know of any future segments,
        // so reset the connection
        if (!seg && nBytesACKed > 0)
        {
            if (con->state.state < TCP_STATE_ESTABLISHED)
            {
                struct tcp_pseudo_hdr resp = {};
                resp.dest_port = con->dest.port;
                resp.src_port = con->src.port;
                resp.flags |= TCP_RST;
                resp.ttl = con->ttl;
                resp.seq = ack;        
                NetH_SendTCPSegment(con->nic, nullptr, con->ip_ent, con->dest.addr, &resp);
                Net_TCPChangeConnectionState(con, TCP_STATE_CLOSED);
                con->reset = true;
            }
            else
            {
                /*
                 * If the connection is in a synchronized state (ESTABLISHED,
                 * "FIN-WAIT-1, FIN-WAIT-2, CLOSE-WAIT, CLOSING, LAST-ACK, TIME-WAIT),
                 * any unacceptable segment (out of window sequence number or
                 * unacceptible acknowledgment number) must elicit only an empty
                 * acknowledgment segment containing the current send-sequence number
                 * and an acknowledgment indicating the next sequence number expected
                 * to be received, and the connection remains in the same state."
                 * (RFC793, 37)
                 */
                struct tcp_pseudo_hdr resp = {};
                resp.ack = con->state.rcv.nxt;
                resp.seq = con->state.snd.nxt;
                resp.dest_port = con->dest.port;
                resp.src_port = con->src.port;
                resp.window = con->state.rcv.wnd;
                resp.flags |= TCP_ACK;
                resp.ttl = con->ttl;
                NetH_SendTCPSegment(con->nic, nullptr, con->ip_ent, con->dest.addr, &resp);
            }
            return false;
        }
    }
    // it's possible that nBytesACKed is >0, if seg was nullptr
    // (i.e., the remote acknoledged a non-existent segment)
    con->state.snd.una -= nBytesACKed;

    Core_PushlockRelease(&con->unacked_segments.lock, true);
    
    seg = LIST_GET_HEAD(tcp_unacked_segment_list, &con->unacked_segments.list);
    while (seg)
    {
        tcp_unacked_segment* const next = LIST_GET_NEXT(tcp_unacked_segment_list, &con->unacked_segments.list, seg);
        
        uint32_t window_edge = con->state.snd.una + con->state.snd.wnd;
        if (!seg->sent && seg->segment.seq < window_edge)
            Net_TCPRetransmitSegment(seg);
        if (seg->expired)
            Net_TCPRetransmitSegment(seg);

        seg = next;
    }

    return true;
}

void Net_TCPRetransmitSegment(tcp_unacked_segment* seg)
{
    OBOS_ASSERT(seg->expired);
    if (!seg->expired && seg->sent)
        return; // why are we doing this if the segment hasn't expired?
    if (seg->nRetries >= TCP_MAX_RETRANSMISSIONS)
    {
        NetDebug(
            "TCP: Cancelling TCP segment after %d retransmissions with no answer\n"
            "ACK=%d, SEQ=%d, DEST.PORT=%d, SRC.PORT=%d, SRC.ADDR=" IP_ADDRESS_FORMAT ", DEST.ADDR=" IP_ADDRESS_FORMAT,
            TCP_MAX_RETRANSMISSIONS,
            seg->segment.ack - seg->con->state.rcv.irs,
            seg->segment.seq - seg->con->state.snd.iss,
            seg->con->src.port,
            IP_ADDRESS_ARGS(seg->con->src.addr),
            seg->con->dest.port,
            IP_ADDRESS_ARGS(seg->con->dest.addr)
        );
        Core_EventSet(&seg->evnt, false);
        seg->expired = true;
        return;
    }

    if (!seg->sent)
    {
        seg->sent = true;
        seg->segment.ack = seg->con->state.rcv.nxt;
    }
    NetH_SendTCPSegment(seg->con->nic, nullptr, seg->con->ip_ent, seg->con->dest.addr, &seg->segment);

    OBOS_SharedPtrRef(&seg->ptr);
    seg->expiration_timer.userdata = seg;
    Core_TimerObjectInitialize(&seg->expiration_timer, TIMER_MODE_DEADLINE, seg->segment.expiration_ms * 1000);
    
    if (!seg->sent)
        seg->sent = true;
    if (seg->expired)
        seg->nRetries++;
    
    seg->expired = false;
}

void Net_TCPPushDataToRemote(tcp_connection* con, const void* buffer, size_t size, bool oob)
{
    // TODO: Urgent data?
    OBOS_UNUSED(oob);

    OBOS_ENSURE(con->state.snd.wnd);

    // NetH_SendTCPSegment handles all queuing, we just need to segment the packet.
    uint32_t window_bytes_until_close = con->state.snd.nxt - (con->state.snd.wnd + con->state.snd.una);
    
    shared_ptr* payload = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(shared_ptr), nullptr);
    OBOS_SharedPtrConstructSz(payload, (void*)buffer, size);
    payload->free = OBOS_SharedPtrDefaultFree;
    payload->freeUdata = OBOS_KernelAllocator;
    payload->onDeref = NetFreeSharedPtr;

    uint32_t offset = 0;
    bool first_iter = true;
    while (size != 0)
    {
        uint32_t nToTransfer = 0;
        if (first_iter)
            nToTransfer = OBOS_MIN(size, window_bytes_until_close);
        else if (size <= con->state.snd.wnd)
            nToTransfer = size;
        else
            nToTransfer = con->state.snd.wnd;

        struct tcp_pseudo_hdr hdr = {};
        if (nToTransfer)
        {
            hdr.payload = OBOS_SharedPtrCopy(payload);
            hdr.payload_offset = offset;
            hdr.payload_size = nToTransfer;
            hdr.check_tx_window = true;
            hdr.dest_port = con->dest.port;
            hdr.src_port = con->src.port;
            hdr.ttl = con->ttl;
            hdr.flags = TCP_ACK;
            if (nToTransfer == size)
                hdr.flags |= TCP_PSH;
            hdr.window = con->state.rcv.wnd;
            hdr.seq = con->state.snd.nxt + offset;
            hdr.ack = con->state.rcv.nxt;
            NetH_SendTCPSegment(con->nic, con, con->ip_ent, con->dest.addr, &hdr);
        }
        
        offset += hdr.payload_size;
        size -= hdr.payload_size;
        first_iter = false;
    }
    
}

void Net_TCPReset(tcp_connection* con)
{
    struct tcp_pseudo_hdr hdr = {};
    hdr.ack = con->state.rcv.nxt;
    hdr.seq = con->state.snd.nxt;
    hdr.flags = TCP_RST;
    hdr.src_port = con->src.port;
    hdr.dest_port = con->dest.port;
    hdr.ttl = con->ttl;
    NetH_SendTCPSegment(con->nic, nullptr, con->ip_ent, con->dest.addr, &hdr);
    Net_TCPChangeConnectionState(con, TCP_STATE_CLOSED);
}

// LIST_GENERATE(tcp_incoming_list, tcp_incoming_packet, node);
RB_GENERATE(tcp_connection_tree, tcp_connection, node, tcp_connection_cmp);
RB_GENERATE(tcp_port_tree, tcp_port, node, tcp_port_cmp);

typedef struct tcp_socket {
    union {
        struct {
            tcp_port** bound_ports;
            size_t bound_port_count;
            event* listen_event;
            tcp_port* interrupted_port;
            thread* internal_listen_thread;
            event internal_listen_event;
            event kill_listen_thread;
        } serv;
        tcp_connection* connection;
    };
    bool is_server : 1;
} tcp_socket;

socket_desc* tcp_create()
{
    socket_desc* ret = Vfs_Calloc(1, sizeof(socket_desc));
    ret->ops = &Net_TCPSocketBackend;
    ret->protocol = IPPROTO_TCP;
    ret->protocol_data = nullptr;
    return ret;
}

void tcp_free(socket_desc* socket)
{
    if (!socket->protocol_data)
        goto down;
    tcp_socket* s = socket->protocol_data;

    if (!s->is_server)
    {
        // Net_TCPSocketBackend.shutdown(socket, SHUT_RDWR);
        // Reset the connection.
        if (s->connection->state.state < TCP_STATE_TIME_WAIT)
            Net_TCPReset(s->connection);
        
        net_tables* iface = s->connection->nic->net_tables;
        Core_PushlockAcquire(&iface->tcp_connections_lock, false);
        if (s->connection->is_client)
            RB_REMOVE(tcp_connection_tree, &iface->tcp_outgoing_connections, s->connection);
        else
        {
            tcp_port key = {.port=s->connection->src.port};
            Core_PushlockAcquire(&iface->tcp_ports_lock, true);
            tcp_port* port = RB_FIND(tcp_port_tree, &iface->tcp_ports, &key);
            Core_PushlockRelease(&iface->tcp_ports_lock, true);
            Core_PushlockAcquire(&port->connection_tree_lock, false);
            RB_REMOVE(tcp_connection_tree, &port->connections, s->connection);
            Core_PushlockRelease(&port->connection_tree_lock, false);
        }
        Core_PushlockRelease(&iface->tcp_connections_lock, false);
        Free(OBOS_KernelAllocator, s->connection, sizeof(*s->connection));
    }
    else
    {
        if (s->serv.internal_listen_thread)
        {
            s->serv.internal_listen_thread->references++;
            Core_EventSet(&s->serv.kill_listen_thread, false);
            while (~s->serv.internal_listen_thread->flags & THREAD_FLAGS_DIED)
                Core_Yield();
            if (!(--s->serv.internal_listen_thread->references) && s->serv.internal_listen_thread->free)
                s->serv.internal_listen_thread->free(s->serv.internal_listen_thread);
        }
        for (size_t i = 0; i < s->serv.bound_port_count; i++)
        {
            tcp_port* port = s->serv.bound_ports[i];
            Core_PushlockAcquire(&port->iface->tcp_ports_lock, false);
            RB_REMOVE(tcp_port_tree, &port->iface->tcp_ports, port);
            Core_PushlockRelease(&port->iface->tcp_ports_lock, false);
            Free(OBOS_KernelAllocator, port, sizeof(*port));    
        }
        Vfs_Free(s->serv.bound_ports);
    }
    Vfs_Free(s);

    down:
    Vfs_Free(socket);
}

obos_status tcp_accept(socket_desc* socket, struct sockaddr* saddr, size_t *addr_len, int flags, bool nonblocking, socket_desc** out)
{
    if (*addr_len < sizeof(struct sockaddr_in))
    {
        *addr_len = sizeof(struct sockaddr_in);
        return OBOS_STATUS_INVALID_ARGUMENT;
    }
    OBOS_UNUSED(flags);
    if (!socket->protocol_data)
        return OBOS_STATUS_UNINITIALIZED;
    tcp_socket* s = socket->protocol_data;
    if (!s->is_server)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (nonblocking && Core_EventGetState(s->serv.listen_event))
        return OBOS_STATUS_WOULD_BLOCK;
    obos_status st = Core_WaitOnObject(WAITABLE_OBJECT(*s->serv.listen_event));
    Core_EventClear(s->serv.listen_event);
    if (obos_is_error(st))
        return st;
    if (!s->serv.interrupted_port)
        return OBOS_STATUS_RETRY;

    tcp_connection* con = nullptr;
    tcp_connection* iter = nullptr;
    Core_PushlockAcquire(&s->serv.interrupted_port->connection_tree_lock, true);
    RB_FOREACH(iter, tcp_connection_tree, &s->serv.interrupted_port->connections)
    {
        if (!iter->accepted)
        {
            con = iter;
            con->accepted = true;
            break;
        }
    }
    Core_PushlockRelease(&s->serv.interrupted_port->connection_tree_lock, true);
    if (s->serv.bound_port_count > 1)
        s->serv.interrupted_port = nullptr;

    *out = tcp_create();
    socket_desc* new_desc = *out;
    tcp_socket* new = new_desc->protocol_data = Vfs_Calloc(1, sizeof(tcp_socket));
    new->is_server = false;
    new->connection = con;
    new->connection->recv_buffer.closed = false;
    struct sockaddr_in* addr = (void*)saddr;
    if (addr)
    {
        *addr_len = sizeof(struct sockaddr_in);
        memcpy(&addr->addr, &con->src.addr, sizeof(ip_addr));
        addr->port = host_to_be16(con->src.port);
        addr->family = AF_INET;
    }

    return st;
}

static obos_status interface_has_address(net_tables* interface, ip_addr addr, ip_table_entry** oent)
{
    Core_PushlockAcquire(&interface->table_lock, true);
    for (ip_table_entry* ent = LIST_GET_HEAD(ip_table, &interface->table); ent; )
    {
        if (ent->address.addr == addr.addr)
        {
            if (oent)
                *oent = ent;
            Core_PushlockRelease(&interface->table_lock, true);
            return OBOS_STATUS_SUCCESS;
        }
        ent = LIST_GET_NEXT(ip_table, &interface->table, ent);
    }
    Core_PushlockRelease(&interface->table_lock, true);
    return OBOS_STATUS_ADDRESS_NOT_AVAILABLE;
}

static obos_status bind_interface(uint16_t port, net_tables* iface, tcp_port** oport)
{
    tcp_port* bport = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(tcp_port), nullptr);
    bport->port = port;
    bport->connection_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    bport->connection_tree_lock = PUSHLOCK_INITIALIZE();
    Core_PushlockAcquire(&iface->tcp_ports_lock, false);
    if (RB_FIND(tcp_port_tree, &iface->tcp_ports, bport))
    {
        Free(OBOS_KernelAllocator, bport, sizeof(*bport));
        Core_PushlockRelease(&iface->tcp_ports_lock, false);
        return OBOS_STATUS_PORT_IN_USE;
    }
    RB_INSERT(tcp_port_tree, &iface->tcp_ports, bport);
    bport->iface = iface;
    Core_PushlockRelease(&iface->tcp_ports_lock, false);
    *oport = bport;
    return OBOS_STATUS_SUCCESS;
}

static void internal_listen_thread(void* udata)
{
    tcp_socket* s = udata;
    struct waitable_header** objs = ZeroAllocate(OBOS_NonPagedPoolAllocator, s->serv.bound_port_count+1, sizeof(struct waitable_header*), nullptr);
    struct waitable_header* signaled = nullptr;
    objs[0] = WAITABLE_OBJECT(s->serv.kill_listen_thread);
    for (size_t i = 1; i <= s->serv.bound_port_count; i++)
        objs[i] = WAITABLE_OBJECT(s->serv.bound_ports[i-1]->connection_event);
    while (1)
    {
        obos_status status = OBOS_STATUS_SUCCESS;
        if (obos_is_error(status = Core_WaitOnObjects(s->serv.bound_port_count+1, objs, &signaled)))
        {
            NetError("Net: %s: Core_WaitOnObjects returned %d, aborting.\n", __func__, status);
            break;
        }

        if (signaled == objs[0])
            break;
        
        for (size_t i = 0; i < s->serv.bound_port_count; i++)
        {
            if (signaled == WAITABLE_OBJECT(s->serv.bound_ports[i]->connection_event))
            {
                Core_EventClear(&s->serv.bound_ports[i]->connection_event);
                s->serv.interrupted_port = s->serv.bound_ports[i];
                break;
            }
        }
        if (!s->serv.interrupted_port)
            continue;
        Core_EventSet(s->serv.listen_event, false);
    }
    Free(OBOS_NonPagedPoolAllocator, objs, (s->serv.bound_port_count+1) * sizeof(struct waitable_header*));
    Core_ExitCurrentThread();
}

obos_status tcp_bind(socket_desc* socket, struct sockaddr* saddr, size_t addr_len)
{
    struct sockaddr_in* addr = (void*)saddr;
    if (addr_len < sizeof(*addr))
        return OBOS_STATUS_INVALID_ARGUMENT;
    uint16_t port = be16_to_host(addr->port);
    if (!port)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (socket->protocol_data)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    tcp_socket* s = Vfs_Calloc(1, sizeof(tcp_socket));
    s->is_server = true;
    s->serv.kill_listen_thread = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    if (addr->addr.addr == 0)
    {
        s->serv.bound_port_count = LIST_GET_NODE_COUNT(network_interface_list, &Net_Interfaces);
        s->serv.bound_ports = Vfs_Calloc(s->serv.bound_port_count, sizeof(tcp_port*));
        size_t i = 0;
        for (net_tables* interface = LIST_GET_HEAD(network_interface_list, &Net_Interfaces); interface && i < s->serv.bound_port_count; i++)
        {
            obos_status status = bind_interface(port, interface, &s->serv.bound_ports[i]);
            if (obos_is_error(status))
            {
                Vfs_Free(s->serv.bound_ports);
                Vfs_Free(s);
                return status;
            }
            interface = LIST_GET_NEXT(network_interface_list, &Net_Interfaces, interface);
        }
    }
    else
    {
        s->serv.bound_port_count = 1;
        s->serv.bound_ports = Vfs_Calloc(s->serv.bound_port_count, sizeof(tcp_port*));
        for (net_tables* interface = LIST_GET_HEAD(network_interface_list, &Net_Interfaces); interface; )
        {
            obos_status status = OBOS_STATUS_SUCCESS;
            status = interface_has_address(interface, addr->addr, nullptr);
            if (obos_is_error(status))
            {
                interface = LIST_GET_NEXT(network_interface_list, &Net_Interfaces, interface);
                continue;
            }

            status = bind_interface(port, interface, &s->serv.bound_ports[0]);
            if (obos_is_error(status))
            {
                Vfs_Free(s->serv.bound_ports);
                Vfs_Free(s);
                return status;
            }
            
            break;
        }
        if (!s->serv.bound_ports)
        {
            Vfs_Free(s->serv.bound_ports);
            Vfs_Free(s);
            return OBOS_STATUS_ADDRESS_NOT_AVAILABLE;
        }
    }

    if (s->serv.bound_port_count == 1)
    {
        s->serv.listen_event = &s->serv.bound_ports[0]->connection_event;
        s->serv.interrupted_port = s->serv.bound_ports[0];
    }
    else
    {
        s->serv.internal_listen_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
        s->serv.listen_event = &s->serv.internal_listen_event;
        s->serv.internal_listen_thread = CoreH_ThreadAllocate(nullptr);
        thread_ctx ctx = {};
        void* stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x1000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr);
        CoreS_SetupThreadContext(&ctx, (uintptr_t)internal_listen_thread, (uintptr_t)s, false, stack, 0x1000);
        CoreH_ThreadInitialize(s->serv.internal_listen_thread, THREAD_PRIORITY_NORMAL, Core_DefaultThreadAffinity, &ctx);
        s->serv.internal_listen_thread->stackFree = CoreH_VMAStackFree;
        s->serv.internal_listen_thread->stackFreeUserdata = &Mm_KernelContext;
        Core_ProcessAppendThread(OBOS_KernelProcess, s->serv.internal_listen_thread);
        CoreH_ThreadReady(s->serv.internal_listen_thread);
    }

    socket->protocol_data = s;

    return OBOS_STATUS_SUCCESS;
}

static obos_status get_src_port(vnode* nic, ip_table_entry* ent, ip_addr dest, uint16_t dest_port, uint16_t *src_port)
{
    tcp_connection key = {
        .dest= {
            .addr=dest,
            .port=dest_port,
        },
        .src= {
            .addr=ent->address,
            .port=*src_port,
        },
        .is_client = true,
    };
    Core_PushlockAcquire(&nic->net_tables->tcp_connections_lock, true);
    tcp_connection* con = *src_port == 0 ? nullptr : RB_FIND(tcp_connection_tree, &nic->net_tables->tcp_outgoing_connections, &key);   
    Core_PushlockRelease(&nic->net_tables->tcp_connections_lock, true);
    if (con)
        return OBOS_STATUS_PORT_IN_USE;

    Core_PushlockAcquire(&nic->net_tables->tcp_connections_lock, true);
    bool found_port = false;
    // bru
    for (int i = 0; i < 0x10000; i++)
    {
        *src_port = mt_random() % 0x10000 + 1;
        key.src.port = *src_port;
        if (!RB_FIND(tcp_connection_tree, &nic->net_tables->tcp_outgoing_connections, &key))
        {
            found_port = true;
            break;
        }
    }
    Core_PushlockRelease(&nic->net_tables->tcp_connections_lock, true);
    if (!found_port)
        return OBOS_STATUS_ADDRESS_IN_USE;
    return OBOS_STATUS_SUCCESS;
}

obos_status tcp_connect(socket_desc* socket, struct sockaddr* saddr, size_t addrlen)
{
    struct sockaddr_in* addr = (struct sockaddr_in*)saddr;
    if (addrlen < sizeof(*addr))
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (socket->protocol_data)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    net_tables* iface = nullptr;
    ip_table_entry* ent = nullptr;
    uint8_t ttl = 0;
    
    obos_status st = NetH_AddressRoute(&iface, &ent, &ttl, addr->addr);
    if (obos_is_error(st))
        return st;

    uint16_t src_port = 0;
    obos_status status = get_src_port(iface->interface, ent, addr->addr, be16_to_host(addr->port), &src_port);
    if (obos_is_error(status))
        return status;

    tcp_socket* s = socket->protocol_data = Vfs_Calloc(1, sizeof(tcp_socket));
    s->is_server = false;
    s->connection = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(tcp_connection), nullptr);
    s->connection->ttl = ttl;
    s->connection->is_client = true;
    s->connection->unacked_segments.lock = PUSHLOCK_INITIALIZE();
    s->connection->nic = iface->interface;
    s->connection->ip_ent = ent;
    s->connection->src.addr = ent->address;
    s->connection->src.port = src_port;
    s->connection->dest.addr = addr->addr;
    s->connection->dest.port = be16_to_host(addr->port);
    s->connection->recv_buffer.size = 0x10000;
    s->connection->state.state_change_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    s->connection->inbound_sig = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    s->connection->inbound_urg_sig = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    s->connection->recv_buffer.lock = MUTEX_INITIALIZE();
    s->connection->state.state = TCP_STATE_SYN_SENT;
    s->connection->state.rcv.wnd = s->connection->recv_buffer.size;
    s->connection->state.rcv.up = 0;
    s->connection->state.snd.iss = (uint32_t)mt_random();
    s->connection->state.snd.nxt = s->connection->state.snd.iss+1;
    s->connection->state.snd.una = s->connection->state.snd.iss;
    s->connection->state.snd.up = 0;

    Core_PushlockAcquire(&iface->tcp_connections_lock, false);
    RB_INSERT(tcp_connection_tree, &iface->tcp_outgoing_connections, s->connection);
    Core_PushlockRelease(&iface->tcp_connections_lock, false);

    struct tcp_pseudo_hdr syn = {};
    syn.dest_port = s->connection->dest.port;
    syn.src_port = s->connection->src.port;
    syn.ttl = s->connection->ttl;
    syn.window = s->connection->state.rcv.wnd;
    syn.flags = TCP_SYN;
    syn.seq = s->connection->state.snd.iss;
    status = NetH_SendTCPSegment(s->connection->nic, s->connection, s->connection->ip_ent, s->connection->dest.addr, &syn);
    if (syn.unacked_seg)
        OBOS_SharedPtrUnref(&syn.unacked_seg->ptr);
    if (obos_is_error(status))
    {
        Core_PushlockAcquire(&iface->tcp_connections_lock, false);
        RB_REMOVE(tcp_connection_tree, &iface->tcp_outgoing_connections, s->connection);
        Core_PushlockRelease(&iface->tcp_connections_lock, false);
        return status;
    }

    while (s->connection->state.state != TCP_STATE_ESTABLISHED)
    {
        status = Core_WaitOnObject(WAITABLE_OBJECT(s->connection->state.state_change_event));
        if (s->connection->reset || obos_is_error(status))
        {
            Core_PushlockAcquire(&iface->tcp_connections_lock, false);
            RB_REMOVE(tcp_connection_tree, &iface->tcp_outgoing_connections, s->connection);
            Core_PushlockRelease(&iface->tcp_connections_lock, false);
            return OBOS_STATUS_CONNECTION_REFUSED;
        }
    }

    return OBOS_STATUS_SUCCESS;
}

obos_status tcp_getpeername(socket_desc* socket, struct sockaddr* saddr, size_t* addrlen)
{
    struct sockaddr_in* addr = (struct sockaddr_in*)saddr;
    if (*addrlen < sizeof(*addr))
        return OBOS_STATUS_INVALID_ARGUMENT;
    *addrlen = sizeof(*addr);
    if (!socket->protocol_data)
        return OBOS_STATUS_UNINITIALIZED;
    tcp_socket* s = socket->protocol_data;
    if (s->is_server)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!s->connection)
        return OBOS_STATUS_UNINITIALIZED;
    if (s->connection->is_client)
    {
        memcpy(&addr->addr, &s->connection->dest.addr, sizeof(s->connection->dest.addr));
        addr->port = host_to_be16(s->connection->dest.port);
    }
    else
    {
        memcpy(&addr->addr, &s->connection->src.addr, sizeof(s->connection->src.addr));
        addr->port = host_to_be16(s->connection->src.port);    
    }
    addr->family = AF_INET;
    memzero(addr->sin_zero, sizeof(addr->sin_zero));
    return OBOS_STATUS_SUCCESS;
}

obos_status tcp_getsockname(socket_desc* socket, struct sockaddr* saddr, size_t* addrlen)
{
    struct sockaddr_in* addr = (struct sockaddr_in*)saddr;
    if (*addrlen < sizeof(*addr))
        return OBOS_STATUS_INVALID_ARGUMENT;
    *addrlen = sizeof(*addr);
    if (!socket->protocol_data)
        return OBOS_STATUS_UNINITIALIZED;
    tcp_socket* s = socket->protocol_data;
    if (s->is_server)
    {
        if (!s->serv.bound_port_count)
            return OBOS_STATUS_INVALID_ARGUMENT;
        ip_addr any = {}; // 0.0.0.0
        if (s->serv.bound_port_count == 1)
            memcpy(&addr->addr, &s->serv.bound_ports[0]->iface->table.head->address, sizeof(addr->addr));
        else
            memcpy(&addr->addr, &any, sizeof(addr->addr));
        addr->port = be16_to_host(s->serv.bound_ports[0]->port);
        addr->family = AF_INET;
        return OBOS_STATUS_SUCCESS;
    }
    if (!s->connection)
        return OBOS_STATUS_UNINITIALIZED;
    if (!s->connection->is_client)
    {
        memcpy(&addr->addr, &s->connection->dest.addr, sizeof(s->connection->dest.addr));
        addr->port = host_to_be16(s->connection->dest.port);
    }
    else
    {
        memcpy(&addr->addr, &s->connection->src.addr, sizeof(s->connection->src.addr));
        addr->port = host_to_be16(s->connection->src.port);    
    }
    addr->family = AF_INET;
    memzero(addr->sin_zero, sizeof(addr->sin_zero));
    return OBOS_STATUS_SUCCESS;
}

obos_status tcp_listen(socket_desc* socket, int backlog)
{
    OBOS_UNUSED(backlog);
    if (!socket->protocol_data)
        return OBOS_STATUS_UNINITIALIZED;
    tcp_socket* s = socket->protocol_data;
    if (!s->is_server)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // No-op
    return OBOS_STATUS_SUCCESS;
}

static void irp_on_event_set(irp* req)
{
    socket_desc* desc = (void*)req->desc;
    tcp_socket* s = desc->protocol_data;
    if (s->connection->reset)
    {
        if (req->evnt)
            Core_EventClear(req->evnt);
        req->status = OBOS_STATUS_ABORTED;
        return;
    }
    size_t read_size = OBOS_MIN(req->blkCount, s->connection->recv_buffer.ptr - s->connection->recv_buffer.in_ptr);
    if ((read_size < req->blkCount && (req->socket_flags & MSG_WAITALL)) || !read_size)
    {
        if (req->evnt)
            Core_EventClear(req->evnt);
        req->status = OBOS_STATUS_IRP_RETRY;
        return;
    }
    req->status = OBOS_STATUS_SUCCESS;
    if (req->dryOp)
        return;

    // if (req->socket_flags)
        //printf("TCP: Recv got 0x%p for flags\n", req->socket_flags);

    Core_MutexAcquire(&s->connection->recv_buffer.lock);

    const void* ptr = s->connection->recv_buffer.buf + s->connection->recv_buffer.in_ptr;
    memcpy(req->buff, ptr, read_size);
    if (~req->socket_flags & MSG_PEEK)
    {
        s->connection->recv_buffer.in_ptr += read_size;
        // printf("TCP: Read %d bytes\n", read_size);
        // printf("TCP window (%d, %d) receive window %d->%d, in_ptr=%d, ptr=%d\n",
        //      s->connection->dest.port, s->connection->src.port,
        //      s->connection->state.rcv.wnd, s->connection->state.rcv.wnd+read_size,
        //      s->connection->recv_buffer.in_ptr,
        //      s->connection->recv_buffer.ptr
        //     );
        s->connection->state.rcv.wnd += read_size;
        OBOS_ENSURE(s->connection->recv_buffer.in_ptr <= s->connection->recv_buffer.ptr);
        if (s->connection->recv_buffer.ptr == s->connection->recv_buffer.in_ptr)
        {
            s->connection->recv_buffer.in_ptr = 0;
            s->connection->recv_buffer.ptr = 0;
            Core_EventClear(&s->connection->inbound_sig);
        }
    }
    
    Core_MutexRelease(&s->connection->recv_buffer.lock);
    req->nBlkRead = read_size;
}

obos_status tcp_submit_irp(irp* req)
{
    socket_desc* desc = (void*)req->desc;
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!desc->protocol_data)
        return OBOS_STATUS_UNINITIALIZED;
    tcp_socket* s = desc->protocol_data;
    if (s->is_server)
    {
        req->status = OBOS_STATUS_SUCCESS;
        req->evnt = s->serv.listen_event;
        return OBOS_STATUS_SUCCESS;
    }
    if (s->connection->recv_buffer.closed && !s->connection->recv_buffer.ptr && req->op == IRP_READ)
    {
        req->status = OBOS_STATUS_SUCCESS;
        req->nBlkRead = 0;
        OBOS_Warning("TCP: Read 0 bytes due to closed connection.\n");
        return OBOS_STATUS_SUCCESS;
    }
    if (s->connection->state.state == TCP_STATE_CLOSED)
    {
        req->status = OBOS_STATUS_UNINITIALIZED;
        return OBOS_STATUS_SUCCESS;
    }
    if (req->op == IRP_READ)
    {
        if (req->blkCount > s->connection->recv_buffer.size)
            req->blkCount = s->connection->recv_buffer.size;
        if ((s->connection->recv_buffer.ptr < req->blkCount && req->socket_flags & MSG_WAITALL) || !s->connection->recv_buffer.ptr)
        {
            req->evnt = &s->connection->inbound_sig;
            req->on_event_set = irp_on_event_set;
        }
        else
            irp_on_event_set(req);
    }
    else
    {
        req->evnt = nullptr;
        req->on_event_set = nullptr;    
    }
    return OBOS_STATUS_SUCCESS;
}

obos_status tcp_finalize_irp(irp* req)
{
    socket_desc* desc = (void*)req->desc;
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!desc->protocol_data)
        return OBOS_STATUS_UNINITIALIZED;
    tcp_socket* s = desc->protocol_data;
    if (s->is_server)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (req->op != IRP_WRITE || req->dryOp)
        return OBOS_STATUS_SUCCESS;
    shared_ptr *payload = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(struct shared_ptr), nullptr);
    OBOS_SharedPtrConstructSz(payload, Allocate(OBOS_NonPagedPoolAllocator, req->blkCount, nullptr), req->blkCount);
    memcpy(payload->obj, req->cbuff, req->blkCount);
    payload->free = OBOS_SharedPtrDefaultFree;
    payload->freeUdata = OBOS_NonPagedPoolAllocator;
    payload->onDeref = NetFreeSharedPtr;

    if (s->connection->state.snd.wnd)
    {
        Net_TCPPushDataToRemote(s->connection, req->cbuff, req->blkCount, req->socket_flags & MSG_OOB);
        req->status = OBOS_STATUS_SUCCESS;
    }
    else
        req->status = OBOS_STATUS_PIPE_CLOSED;

    req->nBlkWritten = req->blkCount;

    return OBOS_STATUS_SUCCESS;
}

obos_status tcp_shutdown(socket_desc* desc, int how)
{
    if (!desc->protocol_data)
        return OBOS_STATUS_UNINITIALIZED;
    tcp_socket* s = desc->protocol_data;
    if (s->is_server)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!s->connection)
        return OBOS_STATUS_UNINITIALIZED;

    if (how == SHUT_RD)
    {
        s->connection->recv_buffer.closed = true;
        return OBOS_STATUS_SUCCESS;
    }
    else if (how == SHUT_RDWR)
        s->connection->recv_buffer.closed = true;

    s->connection->write_closed = true;
    
    net_tables* const iface = s->connection->nic->net_tables;

    switch (s->connection->state.state) {
        case TCP_STATE_CLOSED:
            return OBOS_STATUS_UNINITIALIZED;
        case TCP_STATE_LISTEN:
        case TCP_STATE_SYN_SENT:
        {
            Core_PushlockAcquire(&iface->tcp_connections_lock, false);
            RB_REMOVE(tcp_connection_tree, &iface->tcp_outgoing_connections, s->connection);
            Core_PushlockRelease(&iface->tcp_connections_lock, false);
            break;
        }
        case TCP_STATE_SYN_RECEIVED:
        case TCP_STATE_ESTABLISHED:
        case TCP_STATE_CLOSE_WAIT:
        {
            // well apparently we're supposed to queue this but
            // idk how tf to do that
            // so we're not gonna :)
            struct tcp_pseudo_hdr fin = {};
            fin.ttl = s->connection->ttl;
            fin.dest_port = s->connection->dest.port;
            fin.src_port = s->connection->src.port;
            fin.window = s->connection->state.rcv.wnd;
            fin.seq = s->connection->state.snd.nxt++;
            fin.ack = s->connection->state.rcv.nxt;
            fin.flags = TCP_FIN|TCP_ACK;
            Net_TCPChangeConnectionState(s->connection, TCP_STATE_FIN_WAIT1);
            NetH_SendTCPSegment(s->connection->nic, s->connection, s->connection->ip_ent, s->connection->dest.addr, &fin);
            s->connection->fin_segment = fin.unacked_seg;
            break;
        }
        case TCP_STATE_FIN_WAIT1:
        case TCP_STATE_FIN_WAIT2:
            break; // no-op
        case TCP_STATE_CLOSING:
        case TCP_STATE_LAST_ACK:
        case TCP_STATE_TIME_WAIT:
            return OBOS_STATUS_INVALID_OPERATION;
    }
    

    return OBOS_STATUS_SUCCESS;
}

obos_status tcp_sockatmark(socket_desc* desc)
{
    OBOS_UNUSED(desc);
    // TODO: TCP Urgent Data
    return OBOS_STATUS_RETRY;
}

socket_ops Net_TCPSocketBackend = {
    .proto_type.protocol = IPPROTO_TCP,
    .domain = AF_INET,
    .create = tcp_create,
    .free = tcp_free,
    .accept = tcp_accept,
    .bind = tcp_bind,
    .connect = tcp_connect,
    .getpeername = tcp_getpeername,
    .getsockname = tcp_getsockname,
    .listen = tcp_listen,
    .submit_irp = tcp_submit_irp,
    .finalize_irp = tcp_finalize_irp,
    .shutdown = tcp_shutdown,
    .sockatmark = tcp_sockatmark
};