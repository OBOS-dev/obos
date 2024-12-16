/*
 * drivers/x86/boches_vbe/main.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <arch/x86_64/asm_helpers.h>

#include <scheduler/thread.h>

#include <vfs/vnode.h>
#include <vfs/dirent.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>
#include <driver_interface/pci.h>

#include <uacpi/types.h>

#if !defined(__x86_64__) && !defined(__i686__)
#   error Invalid target for Bochs VBE driver.
#endif

// Defined in suspend.c
void on_wake();
// Defined in suspend.c
void on_suspend();

void cleanup() {}

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
             DRIVER_HEADER_FLAGS_DETECT_VIA_PCI |
             DRIVER_HEADER_HAS_VERSION_FIELD |
             DRIVER_HEADER_PCI_HAS_DEVICE_ID |
             DRIVER_HEADER_PCI_HAS_VENDOR_ID,
    .pciId.indiv = {
        // VGA-Compatible
        .classCode = 0x0003,
        .subClass  = 0x0000,
        .progIf    = 0x0000,
        // Bochs VBE
        .vendorId  = 0x1111,
        .deviceId  = 0x1234,
    },
    .ftable = {
        .driver_cleanup_callback = cleanup,
        // .ioctl = ioctl,
        // .ioctl_var = ioctl_var,
        // .get_blk_size = get_blk_size,
        // .get_max_blk_count = get_max_blk_count,
        // .query_user_readable_name = query_user_readable_name,
        // .foreach_device = foreach_device,
        // .read_sync = read_sync,
        // .write_sync = write_sync,
        .on_suspend = on_suspend,
        .on_wake = on_wake,
    },
    .driverName = "Bochs VBE Driver",
    .version = 1,
    .uacpi_init_level_required = UACPI_INIT_LEVEL_EARLY
};

pci_device* PCIDevice;

static void search_bus(pci_bus* bus)
{
    for (pci_device* dev = LIST_GET_HEAD(pci_device_list, &bus->devices); dev; )
    {
        if (memcmp(&dev->hid, &drv_hdr.pciId, sizeof(pci_hid)))
        {
            PCIDevice = dev;
            break;
        }

        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
    }
}

OBOS_PAGEABLE_FUNCTION driver_init_status OBOS_DriverEntry(driver_id* this)
{
    for (size_t i = 0; i < Drv_PCIBusCount; i++)
        search_bus(&Drv_PCIBuses[i]);
    if (!PCIDevice)
        return (driver_init_status){.status=OBOS_STATUS_NOT_FOUND,.context="Could not find Bochs VBE device.",.fatal=true};
    // Maybe we can add stuff here eventually...
    return (driver_init_status){.status=OBOS_STATUS_SUCCESS};
}
