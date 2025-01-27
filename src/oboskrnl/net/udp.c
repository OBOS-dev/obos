/*
 * oboskrnl/net/udp.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <net/udp.h>
#include <net/ip.h>
#include <net/frame.h>
#include <net/tables.h>

#include <scheduler/schedule.h>

#include <locks/rw_lock.h>
#include <locks/event.h>
#include <locks/wait.h>

#include <allocators/base.h>

#include <stdatomic.h>

#include <utils/list.h>
#include <utils/tree.h>

RB_GENERATE(udp_queue_tree, udp_queue, rb_node, cmp_udp_queue);

OBOS_NO_UBSAN obos_status Net_FormatUDPPacket(udp_header** phdr, const void* data, uint16_t length, uint16_t src_port, uint16_t dest_port)
{
    if (!phdr || !data || !length || !src_port || !dest_port)
        return OBOS_STATUS_INVALID_ARGUMENT;
    udp_header* hdr = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(udp_header)+length, nullptr);
    hdr->length = host_to_be16(length+sizeof(udp_header));
    hdr->dest_port = host_to_be16(dest_port);
    hdr->src_port = host_to_be16(src_port);
    memcpy(hdr+1, data, length);
    *phdr = hdr;
    return OBOS_STATUS_SUCCESS;
}

OBOS_NO_UBSAN obos_status Net_UDPReceiveFrame(const frame* what, const frame* raw_frame, void *ent_)
{
    OBOS_UNUSED(raw_frame);
    udp_header* hdr = (void*)what->buff;
    // TODO: Checksum validation
    if (be16_to_host(hdr->length) > what->sz)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received UDP packet has invalid packet size (NOTE: Buffer overflow). Dropping packet.\n", 
            what->interface_mac_address[0], what->interface_mac_address[1], what->interface_mac_address[2], 
            what->interface_mac_address[3], what->interface_mac_address[4], what->interface_mac_address[5]
        );
        return OBOS_STATUS_INVALID_HEADER;
    }
    if (!hdr->dest_port || !hdr->src_port)
    {
        OBOS_Warning("On NIC %02x:%02x:%02x:%02x:%02x:%02x: Received UDP packet has an invalid destination or source port (NOTE: Port zero is invalid). Dropping packet.\n", 
            what->interface_mac_address[0], what->interface_mac_address[1], what->interface_mac_address[2], 
            what->interface_mac_address[3], what->interface_mac_address[4], what->interface_mac_address[5]
        );
        return OBOS_STATUS_INVALID_HEADER;
    }
    
    udp_queue *queue = NetH_GetUDPQueueForPort(ent_, host_to_be16(hdr->dest_port), false);
    if (!queue)
    {
        // TODO: Send destination unreachable (port unreachable) on ICMP.
        OBOS_Debug("Destination unreachable (port unreachable)\n");
        NetH_ReleaseSharedBuffer(what->base);
        return OBOS_STATUS_NOT_FOUND;
    }
    
    frame* new_frame = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(frame), nullptr);
    *new_frame = *what;
    new_frame->buff += sizeof(*hdr);
    new_frame->sz = be16_to_host(hdr->length) - sizeof(*hdr);
    // printf("%d\n", new_frame->sz);
    new_frame->base->refcount++;
    new_frame->source_port = be16_to_host(hdr->src_port);
    obos_status status = Core_RwLockAcquire(&queue->lock, false);
    if (obos_is_error(status))
    {
        NetH_ReleaseSharedBuffer(new_frame->base);
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, new_frame, sizeof(frame));
        return status;
    }
    LIST_APPEND(frame_queue, &queue->queue, new_frame);
    Core_RwLockRelease(&queue->lock, false);

    Core_EventSet(&queue->recv_event, true);

    return OBOS_STATUS_SUCCESS;
}

OBOS_NO_UBSAN udp_queue* NetH_GetUDPQueueForPort(void* ent_, uint16_t port, bool create)
{
    if (!port || !ent_)
        return nullptr;
    ip_table_entry* ent = ent_;
    // printf("%s of UDP port %d on IP address %d.%d.%d.%d (ent: %p)\n", 
    //     create ? "creation" : "query", 
    //     port,
    //     ent->address.comp1, ent->address.comp2, ent->address.comp3, ent->address.comp4, 
    //     ent
    // );
    Core_RwLockAcquire(&ent->received_udp_packets_tree_lock, true);
    udp_queue what = {.dest_port=port};
    udp_queue* res = RB_FIND(udp_queue_tree, &ent->received_udp_packets, &what);
    Core_RwLockRelease(&ent->received_udp_packets_tree_lock, true);
    if (!create || res)
        return res;

    res = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(udp_queue), nullptr);
    res->dest_port = port;
    res->owning_process = Core_GetCurrentThread()->proc;
    res->lock = RWLOCK_INITIALIZE();
    res->recv_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    Core_RwLockAcquire(&ent->received_udp_packets_tree_lock, false);
    RB_INSERT(udp_queue_tree, &ent->received_udp_packets, res);
    Core_RwLockRelease(&ent->received_udp_packets_tree_lock, false);
    return res;
}

OBOS_NO_UBSAN void NetH_DestroyUDPQueue(void* ent_, udp_queue* queue)
{
    if (!ent_ || !queue)
        return;
    if (atomic_flag_test_and_set(&queue->destroy))
        return;
    ip_table_entry* ent = ent_;
    Core_RwLockAcquire(&queue->lock, false);
    queue->lock.abort = true;
    CoreH_AbortWaitingThreads(WAITABLE_OBJECT(queue->lock));

    for (frame* frame = nullptr; frame; )
    {
        struct frame* const next = LIST_GET_NEXT(frame_queue, &queue->queue, frame);
        NetH_ReleaseSharedBuffer(frame->base);
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, frame, sizeof(*frame));
        
        frame = next;
    }

    RB_REMOVE(udp_queue_tree, &ent->received_udp_packets, queue);
    asm volatile ("" : : :"memory");
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, queue, sizeof(*queue));
}