/*
 * drivers/generic/r8169/CMakeLists.txt
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <klog.h>

#include <driver_interface/header.h>
#include <driver_interface/pci.h>
#include <driver_interface/driverId.h>

#include <allocators/base.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>
#include <scheduler/process.h>
#include <scheduler/thread_context_info.h>

#include <vfs/dirent.h>
#include <vfs/vnode.h>
#include <vfs/irp.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <locks/spinlock.h>

#include "structs.h"

r8169_device* Devices;
size_t nDevices;
driver_id* this_driver;

void on_wake()
{
    for (size_t i = 0; i < nDevices; i++)
    {
        r8169_reset(&Devices[i]);
        r8169_resume_phy(&Devices[i]);
        Devices[i].suspended = false;
    }
}

void on_suspend()
{
    for (size_t i = 0; i < nDevices; i++)
    {
        r8169_save_phy(&Devices[i]);
        Devices[i].suspended = true;
    }
}

obos_status get_blk_size(dev_desc desc, size_t* blkSize)
{
    OBOS_UNUSED(desc);
    if (!blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *blkSize = 1;
    return OBOS_STATUS_SUCCESS;
}

obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    OBOS_UNUSED(desc);
    OBOS_UNUSED(count);
    return OBOS_STATUS_INVALID_OPERATION;
}

obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    OBOS_UNUSED(blkOffset);
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    uint32_t* magic = (void*)desc;
    if (*magic != R8169_HANDLE_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (blkCount > (TX_PACKET_SIZE*128))
        return OBOS_STATUS_INVALID_ARGUMENT;
    r8169_device_handle* hnd = (void*)desc;
    r8169_device* dev = hnd->dev;

    r8169_frame frame = {};
    r8169_frame_generate(dev, &frame, buf, blkCount, FRAME_PURPOSE_TX);
    r8169_buffer_add_frame(&dev->tx_buffer, &frame);
    r8169_tx_queue_flush(dev, true);

    if (nBlkWritten)
        *nBlkWritten = blkCount;

    return OBOS_STATUS_SUCCESS;
}

obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    OBOS_UNUSED(blkOffset);
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    uint32_t* magic = (void*)desc;
    OBOS_ENSURE(*magic == R8169_HANDLE_MAGIC);
    if (*magic != R8169_HANDLE_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (blkCount > RX_PACKET_SIZE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (Core_GetIrql() > IRQL_DISPATCH)
        return OBOS_STATUS_INVALID_IRQL;
    r8169_device_handle* hnd = (void*)desc;
    r8169_device* dev = hnd->dev;

    // Wait for the buffer to receive a frame.
    r8169_buffer_block(&dev->rx_buffer);

    irql oldIrql = Core_SpinlockAcquireExplicit(&dev->rx_buffer_lock, IRQL_R8169, false);

    // printf("rx_curr: %p, rx_off: %d\n", hnd->rx_curr, hnd->rx_off);

    if (!hnd->rx_curr)
        r8169_buffer_read_next_frame(&dev->rx_buffer, &hnd->rx_curr);

    // printf("rx_curr: %p, rx_off: %d\n", hnd->rx_curr, hnd->rx_off);

    if (!hnd->rx_curr)
    {
        if (nBlkRead)
            *nBlkRead = 0;
        Core_SpinlockRelease(&dev->rx_buffer_lock, oldIrql);
        return OBOS_STATUS_SUCCESS;
    }

    size_t szRead = OBOS_MIN(blkCount, hnd->rx_curr->sz - hnd->rx_off);
    Core_SpinlockRelease(&dev->rx_buffer_lock, oldIrql);

    // Lower the IRQL temporarily.
    // This is because there is a chance of a page fault here, which is invalid at IRQL > IRQL_DISPATCH
    memcpy(buf, hnd->rx_curr->buf + hnd->rx_off, szRead);
    hnd->rx_off += szRead;

    // printf("rx_curr: %p, rx_off: %d\n", hnd->rx_curr, hnd->rx_off);

    if (hnd->rx_off >= hnd->rx_curr->sz)
    {
        // printf(__FILE__ ":%d\n", __LINE__);
        oldIrql = Core_SpinlockAcquireExplicit(&dev->rx_buffer_lock, IRQL_R8169, false);
        r8169_frame* next = LIST_GET_NEXT(r8169_frame_list, &dev->rx_buffer.frames, hnd->rx_curr);
        r8169_buffer_remove_frame(&dev->rx_buffer, hnd->rx_curr);
        hnd->rx_curr = next;
        hnd->rx_off = 0;
        Core_SpinlockRelease(&dev->rx_buffer_lock, oldIrql);
    }
    
    // printf("rx_curr: %p, rx_off: %d\n", hnd->rx_curr, hnd->rx_off);

    if (nBlkRead)
        *nBlkRead = szRead;

    return OBOS_STATUS_SUCCESS;
}

OBOS_WEAK obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* u), void* u);

obos_status reference_interface(dev_desc *desc)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    r8169_device* dev = (void*)(*desc);
    if (dev->magic != R8169_DEVICE_MAGIC && dev->magic != R8169_HANDLE_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (dev->magic == R8169_HANDLE_MAGIC)
        dev = ((r8169_device_handle*)(*desc))->dev;

    r8169_device_handle* hnd = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(r8169_device_handle), nullptr);
    *desc = (uintptr_t)hnd;
    hnd->magic = R8169_HANDLE_MAGIC;
    
    dev->refcount++;
    
    hnd->dev = dev;
    
    irql oldIrql = Core_SpinlockAcquireExplicit(&dev->rx_buffer_lock, IRQL_R8169, false);
    hnd->rx_curr = LIST_GET_TAIL(r8169_frame_list, &dev->rx_buffer.frames);
    if (hnd->rx_curr)
        hnd->rx_curr->refcount++;
    Core_SpinlockRelease(&dev->rx_buffer_lock, oldIrql);
    hnd->rx_off = 0;
    
    return OBOS_STATUS_SUCCESS;
}

obos_status unreference_interface(dev_desc desc)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    r8169_device_handle* hnd = (void*)desc;
    if (hnd->magic != R8169_HANDLE_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irql oldIrql = Core_SpinlockAcquireExplicit(&hnd->dev->rx_buffer_lock, IRQL_R8169, false);
    for (r8169_frame* frame = hnd->rx_curr; frame; )
    {
        r8169_frame* next = LIST_GET_NEXT(r8169_frame_list, &hnd->dev->rx_buffer.frames, frame);
        r8169_buffer_remove_frame(&hnd->dev->rx_buffer, frame);
        frame = next;
    }
    hnd->dev->refcount--;
    Core_SpinlockRelease(&hnd->dev->rx_buffer_lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}

obos_status query_user_readable_name(dev_desc what, const char** name)
{
    if (!what)
        return OBOS_STATUS_INVALID_ARGUMENT;
    r8169_device_handle* hnd = (void*)what;
    if (hnd->magic != R8169_HANDLE_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *name = hnd->dev->interface_name;
    return OBOS_STATUS_SUCCESS;
}

obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    OBOS_UNUSED(what,request,argp);
    return OBOS_STATUS_INVALID_IOCTL;
}

void irp_on_rx_event_set(irp* req)
{
    r8169_device_handle* hnd = (void*)req->desc;
    r8169_device* dev = hnd->dev;
    if (!hnd->rx_curr)
        r8169_buffer_read_next_frame(&dev->rx_buffer, &hnd->rx_curr);
    req->nic_data = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(nic_irp_data), nullptr);
    req->nic_data->packet_size = hnd->rx_curr->sz;
    req->status = OBOS_STATUS_SUCCESS;
    if (req->dryOp)
        return;

    size_t szRead = OBOS_MIN(req->blkCount, hnd->rx_curr->sz - hnd->rx_off);
    memcpy(req->buff, hnd->rx_curr->buf + hnd->rx_off, szRead);
    hnd->rx_off += szRead;
    if (hnd->rx_off >= hnd->rx_curr->sz)
    {
        // printf(__FILE__ ":%d\n", __LINE__);
        irql oldIrql = Core_SpinlockAcquireExplicit(&dev->rx_buffer_lock, IRQL_R8169, false);
        r8169_frame* next = LIST_GET_NEXT(r8169_frame_list, &dev->rx_buffer.frames, hnd->rx_curr);
        r8169_buffer_remove_frame(&dev->rx_buffer, hnd->rx_curr);
        hnd->rx_curr = next;
        hnd->rx_off = 0;
        Core_SpinlockRelease(&dev->rx_buffer_lock, oldIrql);
    }

    req->nBlkRead = szRead;
}

obos_status submit_irp(void* req_)
{
    irp* req = req_;
    if (!req)
        return OBOS_STATUS_INVALID_ARGUMENT;

    uint32_t* magic = (void*)req->desc;
    OBOS_ENSURE(*magic == R8169_HANDLE_MAGIC);
    if (*magic != R8169_HANDLE_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (req->blkCount > RX_PACKET_SIZE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (Core_GetIrql() > IRQL_DISPATCH)
        return OBOS_STATUS_INVALID_IRQL;
    r8169_device_handle* hnd = (void*)req->desc;
    r8169_device* dev = hnd->dev;
    
    if (req->op == IRP_READ)
    {
        if (!hnd->rx_curr)
            r8169_buffer_read_next_frame(&dev->rx_buffer, &hnd->rx_curr);
        if (!hnd->rx_curr)
        {
            req->evnt = &dev->rx_buffer.envt;
            req->on_event_set = irp_on_rx_event_set;
        }
    }
    else 
    {
        r8169_frame frame = {};
        r8169_frame_generate(dev, &frame, req->cbuff, req->blkCount, FRAME_PURPOSE_TX);
        r8169_buffer_add_frame(&dev->tx_buffer, &frame);
        r8169_tx_queue_flush(dev, true);
        req->evnt = nullptr;
    }
    return OBOS_STATUS_SUCCESS;
}

// TODO: driver cleanup
void driver_cleanup_callback() {}

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES |
             DRIVER_HEADER_FLAGS_DETECT_VIA_PCI |
             DRIVER_HEADER_HAS_VERSION_FIELD |
             DRIVER_HEADER_PCI_HAS_VENDOR_ID |
             DRIVER_HEADER_PCI_IGNORE_PROG_IF |
             DRIVER_HEADER_PIPE_STYLE_DEVICE,
    .acpiId.nPnpIds = 0,
    .pciId.indiv = {
        .classCode = 0x02  , // Network Controller
        .subClass  = 0x00  , // Ethernet Controller
        .progIf    = 0x00  , // Ignored
        .vendorId  = 0x10ec, // Realtek Semiconductor. Co., Ltd.
        // Verify device IDs at runtime.
    },
    .ftable = {
        .driver_cleanup_callback = driver_cleanup_callback,
        .ioctl = ioctl,
        .get_blk_size = get_blk_size,
        .get_max_blk_count = get_max_blk_count,
        .query_user_readable_name = query_user_readable_name,
        .foreach_device = foreach_device,
        .reference_device = reference_interface,
        .unreference_device = unreference_interface,
        .read_sync = read_sync,
        .write_sync = write_sync,
        .on_wake = on_wake,
        .on_suspend = on_suspend,
    },
    .driverName = "RTL8169 Driver",
    .version = 1,
    .uacpi_init_level_required = PCI_IRQ_UACPI_INIT_LEVEL
};

static uint16_t device_ids[] = {
    0x8161,
    0x8168,
    0x8169,
    0x8136,
};

static void search_bus(pci_bus* bus)
{
    for (pci_device* dev = LIST_GET_HEAD(pci_device_list, &bus->devices); dev; )
    {
        if ((dev->hid.id & 0xffffffff) == (drv_hdr.pciId.id & 0xffffffff))
        {
            // Compare Device IDs.
            bool found = false;
            for (size_t i = 0; i < sizeof(device_ids)/sizeof(device_ids[0]); i++)
            {
                if (dev->hid.indiv.deviceId == device_ids[i])
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
                continue;
            }

            pci_resource* bar0 = nullptr;
            pci_resource* irq_res = nullptr;
            for (pci_resource* res = LIST_GET_HEAD(pci_resource_list, &dev->resources); res; )
            {
                if (res->type == PCI_RESOURCE_BAR && res->bar->idx == 0)
                    bar0 = res;
                if (res->type == PCI_RESOURCE_IRQ)
                    irq_res = res;

                if (bar0 && irq_res)
                    break;

                res = LIST_GET_NEXT(pci_resource_list, &dev->resources, res);
            }

            if (!bar0 || !irq_res)
            {
                OBOS_Warning("%02x:%02x:%02x: Bogus RTL8169 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
                dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
                continue;
            }

            nDevices++;
            Devices = OBOS_NonPagedPoolAllocator->Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(r8169_device), (nDevices-1)*sizeof(r8169_device), nullptr);
            memzero(&Devices[nDevices-1], sizeof(Devices[nDevices-1]));
            Devices[nDevices-1].dev = dev;
            Devices[nDevices-1].bar = bar0;
            Devices[nDevices-1].irq_res = irq_res;
            Devices[nDevices-1].idx = nDevices-1;

            Devices[nDevices-1].tx_count = 0;
            Devices[nDevices-1].tx_bytes = 0;
            Devices[nDevices-1].tx_dropped = 0;
            Devices[nDevices-1].tx_high_priority_awaiting_transfer = 0;
            Devices[nDevices-1].tx_awaiting_transfer = 0;

            Devices[nDevices-1].rx_bytes = 0;
            Devices[nDevices-1].rx_count = 0;
            Devices[nDevices-1].rx_crc_errors = 0;
            Devices[nDevices-1].rx_errors= 0;
            Devices[nDevices-1].rx_length_errors = 0;
            Devices[nDevices-1].rx_dropped = 0;

            Devices[nDevices-1].isr = 0;

            Devices[nDevices-1].suspended = false;
        }

        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
    }
}

OBOS_NO_KASAN void hexdump(void* _buff, size_t nBytes, const size_t width);

OBOS_PAGEABLE_FUNCTION driver_init_status OBOS_DriverEntry(driver_id* this)
{
    this_driver = this;
    for (size_t i = 0; i < Drv_PCIBusCount; i++)
        search_bus(&Drv_PCIBuses[i]);
    if (!nDevices)
        return (driver_init_status){.status=OBOS_STATUS_NOT_FOUND,.fatal=true,.context="Could not find PCI Devices."};

    // Reset the devices.
    for (size_t i = 0; i < nDevices; i++)
    {
        r8169_reset(Devices+i);
        vnode* vn = Drv_AllocateVNode(this_driver, (uintptr_t)&Devices[i], 0, nullptr, VNODE_TYPE_CHR);
        const char* dev_name = Devices[i].interface_name;
        OBOS_Debug("%*s: Registering r8169 NIC card at %s%c%s\n", uacpi_strnlen(this_driver->header.driverName, 64), this_driver->header.driverName, OBOS_DEV_PREFIX, OBOS_DEV_PREFIX[sizeof(OBOS_DEV_PREFIX)-1] == '/' ? 0 : '/', dev_name);
        Drv_RegisterVNode(vn, dev_name);
    }

    return (driver_init_status){.status=OBOS_STATUS_SUCCESS};
}

// hexdump clone lol
// OBOS_NO_KASAN void hexdump(void* _buff, size_t nBytes, const size_t width)
// {
// 	bool printCh = false;
// 	uint8_t* buff = (uint8_t*)_buff;
// 	printf("         Address: ");
// 	for(uint8_t i = 0; i < ((uint8_t)width) + 1; i++)
// 		printf("%02x ", i);
// 	printf("\n%016lx: ", buff);
// 	for (size_t i = 0, chI = 0; i < ((nBytes + 0xf) & ~0xf); i++, chI++)
// 	{
// 		if (printCh)
// 		{
// 			char ch = (i < nBytes) ? buff[i] : 0;
// 			switch (ch)
// 			{
// 			case '\n':
// 			case '\t':
// 			case '\r':
// 			case '\b':
// 			case '\a':
// 			case '\0':
// 			{
// 				ch = '.';
// 				break;
// 			}
// 			default:
// 				break;
// 			}
// 			printf("%c", ch);
// 		}
// 		else
// 			printf("%02x ", (i < nBytes) ? buff[i] : 0);
// 		if (chI == (size_t)(width + (!(i < (width + 1)) || printCh)))
// 		{
// 			chI = 0;
// 			if (!printCh)
// 				i -= (width + 1);
// 			else
// 				printf(" |\n%016lx: ", &buff[i + 1]);
// 			printCh = !printCh;
// 			if (printCh)
// 				printf("\t| ");
// 		}
// 	}
// 	printf("\n");
// }
