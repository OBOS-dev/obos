/*
 * oboskrnl/net/tcp.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

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

obos_status NetH_SendTCPSegment(vnode* nic, void* ent_ /* ip_table_entry */, ip_addr dest, struct tcp_pseudo_hdr* dat)
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
        OBOS_SharedPtrUnref(payload);
    }
    hdr->chksum = tcp_chksum(&ip_psuedo_header, sizeof(ip_psuedo_header), hdr, sz);
    hdr->chksum = be16_to_host(hdr->chksum);
    
    printf("tcp tx segment: hdr->flags=0x%x, hdr->ack=0x%x, hdr->seq=0x%x\n", hdr->flags, be32_to_host(hdr->ack), be32_to_host(hdr->seq));

    return NetH_SendIPv4Packet(nic, ent, dest, 0x6, dat->ttl, 0, OBOS_SharedPtrCopy(ptr));
}

uintptr_t mt_random(void);

obos_status NetH_TCPEstablishConnection(vnode* nic,
                                        void* ent_ /* ip_table_entry */, 
                                        ip_addr dest, 
                                        uint16_t src_port /* optional */, uint16_t dest_port,
                                        uint32_t rx_window_size,
                                        uint16_t mss,
                                        tcp_connection** ocon)
{
    if (!dest_port || !nic || !ocon || !ent_)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // The r8169 can only transmit so much at once...
    // But keep the window size scale code in case.
    if (rx_window_size >= 0x10000)
        return OBOS_STATUS_INVALID_ARGUMENT;

    ip_table_entry* ent = ent_;
    tcp_connection key = {
        .dest= {
            .addr=dest,
            .port=dest_port,
        },
        .src= {
            .addr=ent->address,
            .port=src_port,
        },
        .is_client = true,
    };
    Core_PushlockAcquire(&nic->net_tables->tcp_connections_lock, true);
    tcp_connection* con = src_port == 0 ? nullptr : RB_FIND(tcp_connection_tree, &nic->net_tables->tcp_outgoing_connections, &key);   
    Core_PushlockRelease(&nic->net_tables->tcp_connections_lock, true);
    if (con)
        return OBOS_STATUS_PORT_IN_USE;

    Core_PushlockAcquire(&nic->net_tables->tcp_connections_lock, true);
    bool found_port = false;
    // bru
    for (int i = 0; i < 0x10000; i++)
    {
        src_port = mt_random() % 0x10000 + 1;
        key.src.port = src_port;
        if (!RB_FIND(tcp_connection_tree, &nic->net_tables->tcp_outgoing_connections, &key))
        {
            found_port = true;
            break;
        }
    }
    Core_PushlockRelease(&nic->net_tables->tcp_connections_lock, true);
    if (!found_port)
        return OBOS_STATUS_ADDRESS_IN_USE;

    con = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(tcp_connection), nullptr);
    // Copy identification info first.
    *con = key;
    con->ip_ent = ent;
    con->rx_window = rx_window_size;
    con->state = TCP_STATE_SYN;
    con->last_seq = mt_random() & 0xffffffff;
    con->recv_buffer.size = rx_window_size;
    con->recv_buffer.buf = Mm_QuickVMAllocate(rx_window_size, false);
    con->recv_buffer.ptr = 0;
    con->ttl = 64;
    con->tx_window = 0;
    con->sig = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    con->ack_sig = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    con->nic = nic;

    Core_PushlockAcquire(&nic->net_tables->tcp_connections_lock, false);
    RB_INSERT(tcp_connection_tree, &nic->net_tables->tcp_outgoing_connections, con);
    Core_PushlockRelease(&nic->net_tables->tcp_connections_lock, false);

    int8_t shift = 0;
    uint16_t window = 0;
    if (rx_window_size >= 0x10000)
    {
        shift = 32-__builtin_ctz(rx_window_size); // ceiling(log2(real window size))
        window = rx_window_size / (1<<shift); // real window size / 2^factor
    }
    else
        window = rx_window_size & 0xffff;

    con->rx_window_shift.shift = shift;
    con->rx_window_shift.window = window;

    struct tcp_pseudo_hdr pckt = {};
    pckt.ack = 0;
    pckt.seq = con->last_seq;
    pckt.flags = TCP_SYN;
    pckt.src_port = con->src.port;
    pckt.dest_port = con->dest.port;
    pckt.window = window;
    pckt.ttl = con->ttl;
    if (rx_window_size >= 0x10000)
    {
        // The end option is already initialized.
        pckt.option_list_size = sizeof(struct tcp_option)+1+sizeof(struct tcp_option)+2+1;
        pckt.option_list_size += 3;
        pckt.option_list_size &= ~3;
        struct tcp_option* opts = ZeroAllocate(OBOS_KernelAllocator, 1, pckt.option_list_size, nullptr);
        ((uint8_t*)opts)[pckt.option_list_size-1] = 0;
        memset(opts, 1, pckt.option_list_size);
        opts->data[0] = shift - 16;
        opts->kind = 3; /* window shift */
        opts->len = 3;
        struct tcp_option* opt2 = (void*)((uintptr_t)opts + sizeof(struct tcp_option) + 1);
        opt2->kind = 2;
        opt2->len = 4;
        uint16_t *opt2_data = (void*)opt2->data;
        *opt2_data = host_to_be16(mss);
        pckt.options = opts;
    }
    else {
        pckt.option_list_size = sizeof(struct tcp_option)+2+1;
        pckt.option_list_size += 3;
        pckt.option_list_size &= ~3;
        struct tcp_option* opts = ZeroAllocate(OBOS_KernelAllocator, 1, pckt.option_list_size, nullptr);
        memset(opts, 1, pckt.option_list_size);
        ((uint8_t*)opts)[pckt.option_list_size-1] = 0;
        pckt.options = opts;
        opts->kind = 2;
        opts->len = 4;
        uint16_t *opt_data = (void*)opts->data;
        *opt_data = host_to_be16(mss);
    }
    
    obos_status status = NetH_SendTCPSegment(nic, ent, dest, &pckt);
    if (pckt.options)
        Free(OBOS_KernelAllocator, pckt.options, pckt.option_list_size);

    *ocon = con;

    return status;
}

void tx_tm_hnd(void* udata)
{
    event* evnt = udata;
    Core_EventSet(evnt, false);
}

obos_status NetH_TCPTransmitPacket(vnode* nic, tcp_connection* con, shared_ptr* payload)
{
    size_t nSegments = payload->szObj / con->remote_mss;
    if (payload->szObj % con->remote_mss)
        nSegments++;
    struct tcp_pseudo_hdr *segments = ZeroAllocate(OBOS_KernelAllocator, nSegments, sizeof(struct tcp_pseudo_hdr), nullptr);
    for (size_t i = 0; i < nSegments; i++)
    {
        segments[i].ttl = con->ttl;
        segments[i].window = con->rx_window;
        segments[i].src_port = con->src.port;
        segments[i].dest_port = con->dest.port;
        segments[i].seq = con->last_seq;
        segments[i].ack = con->last_ack;
        segments[i].flags = TCP_ACK|TCP_PSH;
        segments[i].payload = OBOS_SharedPtrCopy(payload);
        segments[i].payload_offset = i * con->remote_mss;
        segments[i].payload_size = i == (nSegments-1) ? payload->szObj % con->remote_mss : con->remote_mss;
        segments[i].options = nullptr;
        segments[i].option_list_size = 0;
        con->last_seq += segments[i].payload_size;
    }
    obos_status status = OBOS_STATUS_SUCCESS;
    for (size_t i = 0; i < nSegments; i++)
    {
        int tries = 10;
        for (; tries >= 0; tries--)
        {
            // payload is unreferenced in NetH_SendTCPSegment
            OBOS_SharedPtrRef(payload);
            status = NetH_SendTCPSegment(nic, con->ip_ent, con->dest.addr, &segments[i]);
            if (obos_is_error(status))
                break;
            event timer_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
            timer timer = {};
            timer.handler = tx_tm_hnd;
            timer.userdata = (void*)&timer_event;
            Core_TimerObjectInitialize(&timer, TIMER_MODE_DEADLINE, 10*1000*1000 /* 10 s */);
            struct waitable_header* objects[2] = {
                WAITABLE_OBJECT(con->ack_sig),
                WAITABLE_OBJECT(timer_event)
            };
            struct waitable_header* signaled = nullptr;
            Core_WaitOnObjects(2, objects, &signaled);
            printf("signaled=%p, ack_sig=%p, timer_event=%p\n", signaled, objects[0], objects[1]);
            Core_CancelTimer(&timer);
            CoreH_FreeDPC(&timer.handler_dpc, false);
            Core_EventClear(&con->ack_sig);
            Core_EventClear(&timer_event);
            if (signaled == objects[0])
                break;
            NetDebug("%s: Retransmission of segment 0x%x (%d tries left)\n", __func__, segments[i].seq, tries);
        }
        if (tries <= 0)
        {
            status = OBOS_STATUS_TIMED_OUT;
            break;
        }
    }
    for (size_t i = 0; i < nSegments; i++)
        OBOS_SharedPtrUnref(payload);
    OBOS_SharedPtrUnref(payload);
    Free(OBOS_KernelAllocator, segments, nSegments);
    return status;
}

obos_status NetH_TCPCloseConnection(vnode* nic, tcp_connection* con)
{
    if (!nic || !con)
        return OBOS_STATUS_INVALID_ARGUMENT;

    con->state = TCP_STATE_FIN_WAIT1;
    
    struct tcp_pseudo_hdr pckt = {};
    pckt.dest_port = con->dest.port;
    pckt.src_port = con->src.port;
    pckt.flags = TCP_FIN|TCP_ACK;
    pckt.ttl = con->ttl;
    pckt.seq = con->last_seq;
    pckt.ack = con->last_ack;
    pckt.window = con->rx_window_shift.window;
    ip_addr dest = con->is_client ? con->dest.addr : con->src.addr;
    return NetH_SendTCPSegment(nic, con->ip_ent, dest, &pckt);
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
        .is_client = true,
    };

    Core_PushlockAcquire(&nic->net_tables->tcp_connections_lock, true);
    tcp_connection* current_connection = RB_FIND(tcp_connection_tree, &nic->net_tables->tcp_outgoing_connections, &conn_key);
    Core_PushlockRelease(&nic->net_tables->tcp_connections_lock, true);

    if (port)
    {
        Core_PushlockAcquire(&port->connection_tree_lock, true);
        current_connection = RB_FIND(tcp_connection_tree, &port->connections, &conn_key);
        Core_PushlockRelease(&port->connection_tree_lock, true);
    }
    
    if ((!current_connection || current_connection->state == TCP_STATE_CLOSED) && !port)
    {
        // Net_ICMPv4DestUnreachable(nic, ip_hdr, (ethernet2_header*)buf->obj, hdr, ICMPv4_CODE_PORT_UNREACHABLE);
        struct tcp_pseudo_hdr resp = {};
        resp.flags = TCP_RST;
        resp.ttl = 64;
        resp.window = 0;
        resp.seq = 1;
        resp.src_port = be16_to_host(hdr->dest_port);
        resp.dest_port = be16_to_host(hdr->src_port);
        NetH_SendTCPSegment(nic, ent, ip_hdr->src_address, &resp);

        NetError("%s: TCP Port %d not bound to any socket.\n", __func__, key.port); 
        Core_PushlockRelease(&nic->net_tables->udp_ports_lock, false);
        ExitPacketHandler();
    }

    struct tcp_pseudo_hdr resp = {};
    resp.flags |= TCP_ACK;
    resp.dest_port = be16_to_host(hdr->src_port);
    resp.src_port = be16_to_host(hdr->dest_port);
    resp.ttl = 64;
    resp.window = !current_connection ? 0 : current_connection->rx_window_shift.window;

    if (hdr->flags & TCP_SYN)
    {
        if (current_connection && current_connection->is_client)
            current_connection->state = TCP_STATE_ESTABLISHED;
        else
        {
            tcp_connection* new_con = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(tcp_connection), nullptr);
            // new_con->incoming_packet_lock = PUSHLOCK_INITIALIZE();
            // new_con->incoming_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
            new_con->last_seq = mt_random() & 0xffffffff;
            new_con->state = TCP_STATE_SYN /* SYN received, waiting for ACK */;
            new_con->sig = EVENT_INITIALIZE(EVENT_NOTIFICATION);
            new_con->ack_sig = EVENT_INITIALIZE(EVENT_NOTIFICATION);
            new_con->dest.addr = ip_hdr->src_address;
            new_con->dest.port = be16_to_host(hdr->src_port);
            new_con->src.addr = ip_hdr->dest_address;
            new_con->src.port = be16_to_host(hdr->dest_port);
            new_con->ttl = 64;
            new_con->rx_window = 4096;
            new_con->recv_buffer.buf = Mm_QuickVMAllocate(4096, false);
            new_con->recv_buffer.size = 4096;
            new_con->recv_buffer.ptr = 0;
            new_con->ip_ent = ent;
            new_con->is_client = false;
            resp.flags |= TCP_SYN;
            current_connection = new_con;
            resp.window = current_connection ? 0 : current_connection->rx_window_shift.window;
            Core_PushlockAcquire(&port->connection_tree_lock, false);
            RB_INSERT(tcp_connection_tree, &port->connections, new_con);
            Core_PushlockRelease(&port->connection_tree_lock, false);
        }
        if ((hdr->data_offset >> 4) > 5)
        {
            // Options present.
            struct tcp_option *curr = (void*)hdr->data;
            bool allow_zero_length = false;
            while (curr->kind)
            {
                switch (curr->kind) {
                    case 3:
                        current_connection->tx_window_shift = (curr->data[0] + 16);
                        break;
                    case 2:
                        current_connection->remote_mss = *(uint16_t*)curr->data;
                        break;
                    case 0:
                    case 1:
                        allow_zero_length = true;
                        break;
                    default: break;
                }
                if (!curr->len)
                {
                    if (allow_zero_length)
                    {
                        curr = (void*)((uintptr_t)curr + 1);
                        break;
                    }
                    else {
                        NetError("%s: %d.%d.%d.%d:%d->%d (SYN): While parsing options, got illegal zero length.\n",
                            __func__,
                            IP_ADDRESS_ARGS(ip_hdr->src_address),
                            be16_to_host(hdr->src_port),
                            be16_to_host(hdr->dest_port)
                        );
                        resp.flags = TCP_RST;
                        goto abort;
                    }
                }
                curr = (void*)((uintptr_t)curr + curr->len);
            }
        }
        current_connection->last_ack = be32_to_host(hdr->seq);
    }

    if (current_connection->tx_window_shift)
        current_connection->tx_window = be16_to_host(hdr->window) * (1 << current_connection->tx_window_shift);
    else
        current_connection->tx_window = be16_to_host(hdr->window);

    printf("tcp recv segment: hdr->flags=0x%x, hdr->ack=0x%x, hdr->seq=0x%x, last_ack=0x%x\n", hdr->flags, be32_to_host(hdr->ack), be32_to_host(hdr->seq), current_connection->last_ack);

    if (be32_to_host(hdr->seq) != current_connection->last_ack)
    {
        NetDebug("hdr->seq=0x%x, last_ack=0x%x\n", be32_to_host(hdr->seq), current_connection->last_ack);
        ExitPacketHandler();
    }

    if (hdr->flags & TCP_ACK)
    {
        if (current_connection->state == TCP_STATE_SYN)
            current_connection->state = TCP_STATE_ESTABLISHED;
        if (hdr->flags == (TCP_ACK|TCP_SYN) || hdr->flags & TCP_FIN)
            resp.seq = ++current_connection->last_seq;
        else
            resp.seq = current_connection->last_seq;
        if (hdr->flags != TCP_ACK)
        {
            resp.ack = be32_to_host(hdr->seq)+1;
            current_connection->last_ack = resp.ack;
        }
        Core_EventSet(&current_connection->ack_sig, false);
    }

    if ((be32_to_host(hdr->seq) - resp.ack) > 0)
    {
        size_t data_sent = be16_to_host(ip_hdr->packet_length) - IPv4_GET_HEADER_LENGTH(ip_hdr) - ((hdr->data_offset >> 4) * 4);
        uint32_t nRead = OBOS_MIN(current_connection->recv_buffer.size - current_connection->recv_buffer.ptr, data_sent);
        void* recv_data = (void*)(((uintptr_t)hdr) + ((hdr->data_offset >> 4) * 4));
        memcpy(current_connection->recv_buffer.buf + current_connection->recv_buffer.ptr, recv_data, nRead);
        current_connection->recv_buffer.ptr += nRead;
        resp.ack = be32_to_host(hdr->seq) + nRead;
        current_connection->last_ack = resp.ack;
        Core_EventSet(&current_connection->sig, true);
    }

    switch (current_connection->state) {
        case TCP_STATE_FIN_WAIT1:
            if (hdr->flags & TCP_FIN)
            {
                current_connection->state = TCP_STATE_CLOSED;
                Core_EventSet(&current_connection->sig, false);
                NetH_SendTCPSegment(nic, ent, ip_hdr->src_address, &resp);
                ExitPacketHandler();
            }
            break;
        case TCP_STATE_FIN_WAIT2:
            if (hdr->flags & TCP_ACK)
            {
                current_connection->state = TCP_STATE_CLOSED;
                Core_EventSet(&current_connection->sig, false);    
                NetH_SendTCPSegment(nic, ent, ip_hdr->src_address, &resp);
                ExitPacketHandler();    
            }
            break;
        default:
            if (hdr->flags & TCP_FIN)
            {
                current_connection->state = TCP_STATE_FIN_WAIT2;
                resp.flags |= TCP_FIN;
            }
            break;
    }

    if (hdr->flags & TCP_RST)
    {
        current_connection->state = TCP_STATE_CLOSED;
        current_connection->reset = true;
        Core_EventSet(&current_connection->sig, false);    
    }
    
    abort:

    if (current_connection->state < TCP_STATE_CLOSED)
        NetH_SendTCPSegment(nic, ent, ip_hdr->src_address, &resp);

    if (resp.flags == TCP_RST)
        current_connection->state = TCP_STATE_CLOSED;

    ExitPacketHandler();
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
    bool recv_closed : 1;
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
        Net_TCPSocketBackend.shutdown(socket, SHUT_RDWR);
        net_tables* iface = s->connection->nic->net_tables;
        Core_PushlockAcquire(&iface->tcp_connections_lock, false);
        RB_REMOVE(tcp_connection_tree, &iface->tcp_outgoing_connections, s->connection);
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
            Vfs_Free(port);
        }
        Vfs_Free(s->serv.bound_ports);
    }
    Vfs_Free(s);

    down:
    Vfs_Free(socket);
}

obos_status tcp_accept(socket_desc* socket, struct sockaddr_in* addr, int flags, socket_desc** out)
{
    OBOS_UNUSED(flags);
    if (!socket->protocol_data)
        return OBOS_STATUS_UNINITIALIZED;
    tcp_socket* s = socket->protocol_data;
    if (!s->is_server)
        return OBOS_STATUS_INVALID_ARGUMENT;
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
    new->recv_closed = false;
    new->connection = con;
    memcpy(&addr->addr, &con->src.addr, sizeof(ip_addr));
    addr->port = host_to_be16(con->src.port);
    addr->family = AF_INET;

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

void internal_listen_thread(void* udata)
{
    tcp_socket* s = udata;
    struct waitable_header** objs = ZeroAllocate(OBOS_NonPagedPoolAllocator, s->serv.bound_port_count+1, sizeof(struct waitable_header*), nullptr);
    struct waitable_header* signaled = nullptr;
    objs[0] = WAITABLE_OBJECT(s->serv.kill_listen_thread);
    for (size_t i = 1; i <= s->serv.bound_port_count; i++)
        objs[i] = WAITABLE_OBJECT(s->serv.bound_ports[i-1]->connection_event);
    while (1)
    {
        Core_WaitOnObjects(s->serv.bound_port_count+1, objs, &signaled);
        if (signaled == objs[0])
            break;
        for (size_t i = 0; i < s->serv.bound_port_count; i++)
        {
            if (signaled == WAITABLE_OBJECT(s->serv.bound_ports[i]->connection_event))
            {
                s->serv.interrupted_port = s->serv.bound_ports[i];
                break;
            }
        }
        if (!s->serv.interrupted_port)
            continue;
        Core_EventSet(s->serv.listen_event, false);
    }
    Free(OBOS_NonPagedPoolAllocator, objs, s->serv.bound_port_count+1 * sizeof(struct waitable_header*));
    Core_ExitCurrentThread();
}

obos_status tcp_bind(socket_desc* socket, struct sockaddr_in* addr)
{
    uint16_t port = be16_to_host(addr->port);
    if (!port)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (socket->protocol_data)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    tcp_socket* s = Vfs_Calloc(1, sizeof(tcp_socket));
    s->is_server = true;
    s->recv_closed = false;
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

obos_status tcp_connect(socket_desc* socket, struct sockaddr_in* addr)
{
    if (socket->protocol_data)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    net_tables* iface = nullptr;
    ip_table_entry* ent = nullptr;
    uint8_t ttl = 0;
    
    obos_status st = NetH_AddressRoute(&iface, &ent, &ttl, addr->addr);
    if (obos_is_error(st))
        return st;

    tcp_socket* s = socket->protocol_data = Vfs_Calloc(1, sizeof(tcp_socket));
    s->is_server = false;
    st = NetH_TCPEstablishConnection(
        iface->interface, 
        ent, 
        addr->addr, 
        0,
        be16_to_host(addr->port),
        1024, 
        1520, 
        &s->connection);
    if (obos_is_error(st))
    {
        Vfs_Free(s);
        socket->protocol_data = nullptr;
        return st;
    }
    event tm_sig = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    timer tm = {};
    tm.userdata = (void*)&tm_sig;
    tm.handler = tx_tm_hnd;
    Core_TimerObjectInitialize(&tm, TIMER_MODE_DEADLINE, 10*1000*1000);
    
    struct waitable_header* objs[3] = {
        WAITABLE_OBJECT(s->connection->ack_sig),
        WAITABLE_OBJECT(s->connection->sig),
        WAITABLE_OBJECT(tm_sig)
    };
    struct waitable_header* signaled = nullptr;
    while (s->connection->state != TCP_STATE_ESTABLISHED)
    {
        Core_WaitOnObjects(sizeof(objs)/sizeof(*objs), objs, &signaled);
        if (signaled == WAITABLE_OBJECT(tm_sig) || s->connection->reset)
        {
            Core_PushlockAcquire(&iface->tcp_connections_lock, false);
            RB_REMOVE(tcp_connection_tree, &iface->tcp_outgoing_connections, s->connection);
            Core_PushlockRelease(&iface->tcp_connections_lock, false);
            Free(OBOS_KernelAllocator, s->connection, sizeof(*s->connection));
            
            Vfs_Free(s);
            Core_CancelTimer(&tm);
            CoreH_FreeDPC(&tm.handler_dpc, false);
            socket->protocol_data = nullptr;
            return OBOS_STATUS_CONNECTION_REFUSED;
        }
        Core_EventClear(&s->connection->ack_sig);
        Core_EventClear(&s->connection->sig);
    }
    Core_CancelTimer(&tm);
    CoreH_FreeDPC(&tm.handler_dpc, false);

    return OBOS_STATUS_SUCCESS;
}

obos_status tcp_getpeername(socket_desc* socket, struct sockaddr_in* addr)
{
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

obos_status tcp_getsockname(socket_desc* socket, struct sockaddr_in* addr)
{
    if (!socket->protocol_data)
        return OBOS_STATUS_UNINITIALIZED;
    tcp_socket* s = socket->protocol_data;
    if (s->is_server)
        return OBOS_STATUS_INVALID_ARGUMENT;
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
    if ((s->connection->recv_buffer.ptr < req->blkCount && (req->socket_flags & MSG_WAITALL)) || !s->connection->recv_buffer.ptr)
    {
        if (req->evnt)
            Core_EventClear(req->evnt);
        req->status = OBOS_STATUS_IRP_RETRY;
        return;
    }
    req->status = OBOS_STATUS_SUCCESS;
    if (req->dryOp)
        return;
    size_t nToRead = OBOS_MIN(s->connection->recv_buffer.ptr, req->blkCount);
    memcpy(req->buff, s->connection->recv_buffer.buf, nToRead);
    s->connection->recv_buffer.ptr -= nToRead;
    req->nBlkRead += nToRead;
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
        req->status = OBOS_STATUS_INVALID_ARGUMENT;
        return OBOS_STATUS_SUCCESS;
    }
    if (s->recv_closed && req->op == IRP_READ)
    {
        req->status = OBOS_STATUS_UNINITIALIZED;
        return OBOS_STATUS_SUCCESS;
    }
    if (s->connection->state == TCP_STATE_CLOSED)
    {
        req->status = OBOS_STATUS_UNINITIALIZED;
        return OBOS_STATUS_SUCCESS;
    }
    if (req->op == IRP_READ)
    {
        if (req->blkCount > s->connection->recv_buffer.size)
        {
            req->status = OBOS_STATUS_MESSAGE_TOO_BIG;
            return OBOS_STATUS_SUCCESS;
        }
        if (s->connection->recv_buffer.ptr < req->blkCount)
        {
            req->evnt = &s->connection->sig;
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
    req->status = NetH_TCPTransmitPacket(s->connection->nic, s->connection, OBOS_SharedPtrCopy(payload));
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

    if (how == SHUT_RD)
    {
        s->recv_closed = true;
        return OBOS_STATUS_SUCCESS;
    }
    else if (how == SHUT_RDWR)
        s->recv_closed = true;

    obos_status st = NetH_TCPCloseConnection(s->connection->nic, s->connection);
    if (obos_is_error(st))
        return st;

    event tm_sig = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    timer tm = {};
    tm.userdata = (void*)&tm_sig;
    tm.handler = tx_tm_hnd;
    Core_TimerObjectInitialize(&tm, TIMER_MODE_DEADLINE, 10*1000*1000);
    
    struct waitable_header* objs[2] = {
        WAITABLE_OBJECT(s->connection->ack_sig),
        WAITABLE_OBJECT(tm_sig)
    };
    Core_EventClear(&s->connection->ack_sig);
    struct waitable_header* signaled = nullptr;
    while (s->connection->state != TCP_STATE_CLOSED)
    {
        Core_WaitOnObjects(2, objs, &signaled);
        if (signaled == WAITABLE_OBJECT(tm_sig))
        {
            s->connection->state = TCP_STATE_CLOSED;
            break;
        }
        Core_EventClear(&s->connection->ack_sig);
    }
    Core_CancelTimer(&tm);
    CoreH_FreeDPC(&tm.handler_dpc, false);

    return OBOS_STATUS_SUCCESS;
}

obos_status tcp_sockatmark(socket_desc* desc)
{
    OBOS_UNUSED(desc);
    // TODO: TCP Urgent Data
    return OBOS_STATUS_RETRY;
}

socket_ops Net_TCPSocketBackend = {
    .protocol = IPPROTO_TCP,
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