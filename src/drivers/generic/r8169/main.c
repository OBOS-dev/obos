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

#include "scheduler/schedule.h"
#include "structs.h"

OBOS_WEAK void on_wake();
OBOS_WEAK void on_suspend();
OBOS_WEAK obos_status get_blk_size(dev_desc desc, size_t* blkSize);
OBOS_WEAK obos_status get_max_blk_count(dev_desc desc, size_t* count);
OBOS_WEAK obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead);
OBOS_WEAK obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);
OBOS_WEAK obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* u), void* u);
OBOS_WEAK obos_status query_user_readable_name(dev_desc what, const char** name); // unrequired for fs drivers.
OBOS_WEAK obos_status ioctl(dev_desc what, uint32_t request, void* argp);
OBOS_WEAK void driver_cleanup_callback() {}
__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES |
             DRIVER_HEADER_FLAGS_DETECT_VIA_PCI |
             DRIVER_HEADER_HAS_VERSION_FIELD |
             DRIVER_HEADER_PCI_HAS_VENDOR_ID |
             DRIVER_HEADER_PCI_IGNORE_PROG_IF,
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

r8169_device* Devices;
size_t nDevices;
driver_id* this_driver;

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
                dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
                continue;
            }

            nDevices++;
            Devices = OBOS_KernelAllocator->Reallocate(OBOS_KernelAllocator, Devices, nDevices*sizeof(r8169_device), (nDevices-1)*sizeof(r8169_device), nullptr);
            Devices[nDevices-1].dev = dev;
            Devices[nDevices-1].bar = bar0;
            Devices[nDevices-1].irq_res = irq_res;
            memzero(Devices[nDevices-1].mac, sizeof(Devices[nDevices-1].mac));
            // Zeroed in reset_dev.
            // memzero(Devices[nDevices-1].mac_readable, sizeof(Devices[nDevices-1].mac_readable));
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
        r8169_reset(Devices+i);

    Devices[0].refcount++;
    r8169_buffer_block(&Devices[0].rx_buffer);
    Core_PushlockAcquire(&Devices[0].rx_buffer_lock, true);
    r8169_frame* frame = nullptr;
    r8169_buffer_read_next_frame(&Devices[0].rx_buffer, &frame);
    hexdump(frame->buf, frame->sz, 15);

    Core_SuspendScheduler(true);
    Core_WaitForSchedulerSuspend();

    return (driver_init_status){.status=OBOS_STATUS_SUCCESS};
}

// hexdump clone lol
OBOS_NO_KASAN void hexdump(void* _buff, size_t nBytes, const size_t width)
{
	bool printCh = false;
	uint8_t* buff = (uint8_t*)_buff;
	printf("         Address: ");
	for(uint8_t i = 0; i < ((uint8_t)width) + 1; i++)
		printf("%02x ", i);
	printf("\n%016lx: ", buff);
	for (size_t i = 0, chI = 0; i < nBytes; i++, chI++)
	{
		if (printCh)
		{
			char ch = buff[i];
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
			printf("%02x ", buff[i]);
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
