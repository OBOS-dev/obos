/*
 * oboskrnl/net/udp.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <net/macros.h>
#include <net/udp.h>
#include <net/ip.h>
#include <net/tables.h>
#include <net/icmp.h>

#include <allocators/base.h>

#include <utils/list.h>

#if __x86_64__
#   include <arch/x86_64/gdbstub/connection.h>
#   include <arch/x86_64/gdbstub/debug.h>
#   include <scheduler/thread.h>
#   include <scheduler/process.h>
#   include <mm/context.h>
#   include <mm/alloc.h>
static void kdbg_breaker_thread()
{
    Kdbg_Break();
    Core_ExitCurrentThread();
}
#endif

#include <scheduler/cpu_local.h>

static void pckt_onDeref(struct shared_ptr* ptr)
{
    udp_recv_packet* pckt = ptr->obj;
    if (ptr->refs == 0)
    {
		OBOS_SharedPtrUnref(&pckt->buffer_ptr);
        LIST_REMOVE(udp_recv_packet_list, &pckt->bound_to->packets, pckt);
    }
}

PacketProcessSignature(UDP, ip_header*)
{
    OBOS_UNUSED(depth && size);
    udp_header* hdr = ptr;
    ip_header* ip_hdr = userdata;
    void* udp_pckt_data = hdr+1;
    size_t udp_pckt_sz = be16_to_host(hdr->length) - sizeof(udp_header);
    udp_port key = {.port=be16_to_host(hdr->dest_port)};
    Core_PushlockAcquire(&nic->net_tables->udp_ports_lock, false);
    udp_port* dest = RB_FIND(udp_port_tree, &nic->net_tables->udp_ports, &key);
    if (!dest)
    {
        Net_ICMPv4DestUnreachable(nic, ip_hdr, (ethernet2_header*)buf->obj, hdr, ICMPv4_CODE_PORT_UNREACHABLE);
        NetError("%s: UDP Port %d not bound to any socket.\n", __func__, key.port); 
        Core_PushlockRelease(&nic->net_tables->udp_ports_lock, false);
        ExitPacketHandler();
    }

    udp_recv_packet *pckt = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(udp_recv_packet), nullptr);
    
    OBOS_SharedPtrConstruct(&pckt->packet_ptr, pckt);
    pckt->packet_ptr.free = OBOS_SharedPtrDefaultFree;
    pckt->packet_ptr.freeUdata = OBOS_KernelAllocator;
    pckt->packet_ptr.onDeref = pckt_onDeref;
    OBOS_SharedPtrRef(&pckt->packet_ptr);
    
    OBOS_SharedPtrConstructSz(
        &pckt->buffer_ptr, 
        memcpy(Allocate(OBOS_KernelAllocator, udp_pckt_sz, nullptr), udp_pckt_data, udp_pckt_sz), 
        udp_pckt_sz);
    pckt->buffer_ptr.free = OBOS_SharedPtrDefaultFree;
    pckt->buffer_ptr.freeUdata = OBOS_KernelAllocator;
    OBOS_SharedPtrRef(&pckt->buffer_ptr);
    
    pckt->src.addr = ip_hdr->src_address;
    pckt->src.port = be16_to_host(hdr->src_port);

    pckt->bound_to = dest;
    
    LIST_APPEND(udp_recv_packet_list, &dest->packets, pckt);
    
    Core_EventSet(&dest->recv_event, false);
    
    Core_PushlockRelease(&nic->net_tables->udp_ports_lock, false);
    
    #if __x86_64__
        if (memcmp(udp_pckt_data, "\x03", 1) && udp_pckt_sz == 1)
        {
            // Kdbg break?
            if (Kdbg_CurrentConnection && Kdbg_CurrentConnection->connection_active && !Kdbg_Paused)
            {
                // We need to do this because this thread can't be blocked without
                // removing internet access, and because the idle thread cannot be
                // blocked.
                thread_ctx ctx = {};
                void* stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x4000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr);
                CoreS_SetupThreadContext(
                    &ctx,
                    (uintptr_t)kdbg_breaker_thread, 0, 
                    false, 
                    stack, 0x4000);
                thread* thr = CoreH_ThreadAllocate(nullptr);
                CoreH_ThreadInitialize(thr, THREAD_PRIORITY_NORMAL, Core_DefaultThreadAffinity, &ctx);
                Core_ProcessAppendThread(OBOS_KernelProcess, thr);
                thr->stackFree = CoreH_VMAStackFree;
                thr->stackFreeUserdata = &Mm_KernelContext;
                CoreH_ThreadReady(thr);
            }
        }
    #endif

    ExitPacketHandler();
}

LIST_GENERATE(udp_recv_packet_list, udp_recv_packet, node);
RB_GENERATE(udp_port_tree, udp_port, node, udp_port_cmp);