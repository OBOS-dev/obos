/*
 * oboskrnl/net/route.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <vfs/irp.h>
#include <vfs/mount.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <net/eth.h>
#include <net/tables.h>
#include <net/macros.h>

#include <locks/pushlock.h>

#include <utils/shared_ptr.h>
#include <utils/tree.h>
#include <utils/list.h>

#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>
#include <scheduler/process.h>

#include <allocators/base.h>

DefineNetFreeSharedPtr

static void dispatcher(vnode* nic)
{
    net_tables* tables = nic->net_tables;
    obos_status status = OBOS_STATUS_SUCCESS;
    while (!tables->kill_dispatch)
    {
        irp* req = VfsH_IRPAllocate();
        req->blkCount = 0;
        req->blkOffset = 0;
        req->vn = nic;
        req->dryOp = true;
        req->op = IRP_READ;
        VfsH_IRPSubmit(req, &tables->desc);
        if (obos_is_error(status = VfsH_IRPWait(req)))
        {
            OBOS_Error("%s@%02x:%02x:%02x:%02x:%02x:%02x:, VfsH_IRPWait: Status %d\n", 
                __func__,
                tables->mac[0], tables->mac[1], tables->mac[2], 
                tables->mac[3], tables->mac[4], tables->mac[5],
                status
            );
            VfsH_IRPUnref(req);
            continue;
        }
        // nic_irp_data* data = req->nic_data;
        req->evnt = nullptr;
        req->drvData = nullptr;
        req->on_event_set = nullptr;
        req->blkCount = req->nBlkRead;
        req->nBlkRead = 0;
        req->buff = Allocate(OBOS_KernelAllocator, req->blkCount, nullptr);
        // req->blkCount = data->packet_size;
        req->dryOp = false;
        VfsH_IRPSubmit(req, &tables->desc);
        VfsH_IRPWait(req);

        shared_ptr* buf = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(shared_ptr), nullptr);
        OBOS_SharedPtrConstructSz(buf, req->buff, req->blkCount);
        buf->free = OBOS_SharedPtrDefaultFree;
        buf->onDeref = NetFreeSharedPtr;
        buf->freeUdata = OBOS_KernelAllocator;

        int depth = -1;
        InvokePacketHandler(Ethernet, buf->obj, buf->szObj, nullptr);
        
        VfsH_IRPUnref(req);
    }
    Core_ExitCurrentThread();
}

obos_status Net_Initialize(vnode* nic)
{
    if (!nic)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (nic->net_tables)
        return OBOS_STATUS_ALREADY_INITIALIZED;

    nic->net_tables = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(net_tables), nullptr);

    mount* const point = nic->mount_point ? nic->mount_point : nic->un.mounted;
    const driver_header* driver = nic->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (nic->vtype == VNODE_TYPE_CHR || nic->vtype == VNODE_TYPE_BLK || nic->vtype == VNODE_TYPE_FIFO)
        driver = &nic->un.device->driver->header;
    nic->net_tables->desc = nic->desc;
    if (driver->ftable.reference_device)
        driver->ftable.reference_device(&nic->net_tables->desc);

    driver->ftable.ioctl(nic->net_tables->desc, IOCTL_ETHERNET_INTERFACE_MAC_REQUEST, &nic->net_tables->mac);

    nic->net_tables->dispatch_thread = CoreH_ThreadAllocate(nullptr);
    thread_ctx ctx = {};
    void* stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x4000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr);
    CoreS_SetupThreadContext(&ctx, (uintptr_t)dispatcher, (uintptr_t)nic, false, stack, 0x4000);
    nic->net_tables->dispatch_thread->stackFree = CoreH_VMAStackFree;
    nic->net_tables->dispatch_thread->stackFreeUserdata = &Mm_KernelContext;
    CoreH_ThreadInitialize(nic->net_tables->dispatch_thread, THREAD_PRIORITY_HIGH, Core_DefaultThreadAffinity, &ctx);
    Core_ProcessAppendThread(OBOS_KernelProcess, nic->net_tables->dispatch_thread);
    CoreH_ThreadReady(nic->net_tables->dispatch_thread);

    nic->net_tables->arp_cache_lock = PUSHLOCK_INITIALIZE();
    nic->net_tables->table_lock = PUSHLOCK_INITIALIZE();
    nic->net_tables->fragmented_packets_lock = PUSHLOCK_INITIALIZE();
    nic->net_tables->udp_ports_lock = PUSHLOCK_INITIALIZE();
    nic->net_tables->tcp_connections_lock = PUSHLOCK_INITIALIZE();
    nic->net_tables->tcp_ports_lock = PUSHLOCK_INITIALIZE();
    nic->net_tables->interface = nic;
    nic->net_tables->magic = IP_TABLES_MAGIC;

    return OBOS_STATUS_SUCCESS;
}
obos_status NetH_SendEthernetPacket(vnode *nic, shared_ptr* data)
{
    if (!nic || !data)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!nic->net_tables || !data->obj || !data->szObj)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (nic->net_tables->magic != IP_TABLES_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;

    irp* req = VfsH_IRPAllocate();
    req->vn = nic;
    VfsH_IRPBytesToBlockCount(nic, data->szObj, &req->blkCount);
    req->cbuff = data->obj;
    req->op = IRP_WRITE;
    req->blkOffset = 0;
    VfsH_IRPSubmit(req, &nic->net_tables->desc);
    VfsH_IRPWait(req);
    OBOS_SharedPtrUnref(data);
    VfsH_IRPUnref(req);
    return OBOS_STATUS_SUCCESS;
}

LIST_GENERATE(gateway_list, gateway, node);
LIST_GENERATE(ip_table, ip_table_entry, node);
RB_GENERATE(address_table, address_table_entry, node, cmp_address_table_entry);

bool NetH_NetworkErrorLogsEnabled()
{
    static bool initialized_opt = 0, opt_val = 0;
    if (!initialized_opt)
    {
        opt_val = OBOS_GetOPTF("disable-network-error-logs");
        initialized_opt = true;
    }
    return opt_val;
}