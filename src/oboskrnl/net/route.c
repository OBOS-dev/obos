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

#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>

#include <allocators/base.h>

// hexdump clone lol
OBOS_NO_KASAN void hexdump(void* _buff, size_t nBytes, const size_t width)
{
	bool printCh = false;
	uint8_t* buff = (uint8_t*)_buff;
	printf("         Address: ");
	for(uint8_t i = 0; i < ((uint8_t)width) + 1; i++)
		printf("%02x ", i);
	printf("\n%016lx: ", buff);
	for (size_t i = 0, chI = 0; i < ((nBytes + 0xf) & ~0xf); i++, chI++)
	{
		if (printCh)
		{
			char ch = (i < nBytes) ? buff[i] : 0;
			switch (ch)
			{
			case '\n':
			case '\t':
			case '\r':
			case '\b':
			case '\a':
			case '\0':
			{
				ch = '.';
				break;
			}
			default:
				break;
			}
			printf("%c", ch);
		}
		else
			printf("%02x ", (i < nBytes) ? buff[i] : 0);
		if (chI == (size_t)(width + (!(i < (width + 1)) || printCh)))
		{
			chI = 0;
			if (!printCh)
				i -= (width + 1);
			else
				printf(" |\n%016lx: ", &buff[i + 1]);
			printCh = !printCh;
			if (printCh)
				printf("\t| ");
		}
	}
	printf("\n");
}

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
        hexdump(req->buff, req->nBlkRead, 16);
        Free(OBOS_KernelAllocator, req->buff, req->blkCount);
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
    CoreH_ThreadReady(nic->net_tables->dispatch_thread);

    return OBOS_STATUS_SUCCESS;
}