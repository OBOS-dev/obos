/*
 * drivers/generic/ehci/main.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <error.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>
#include <driver_interface/pci.h>

#include <utils/list.h>

#include <allocators/base.h>

#include "structs.h"

OBOS_WEAK obos_status get_blk_size(dev_desc desc, size_t* blkSize);
OBOS_WEAK obos_status get_max_blk_count(dev_desc desc, size_t* count);
OBOS_WEAK obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead);
OBOS_WEAK obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);
OBOS_WEAK obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* u), void* u);
OBOS_WEAK obos_status query_user_readable_name(dev_desc what, const char** name);
OBOS_WEAK obos_status submit_irp(void*);
OBOS_WEAK obos_status finalize_irp(void*);
OBOS_PAGEABLE_FUNCTION obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    OBOS_UNUSED(what);
    OBOS_UNUSED(request);
    OBOS_UNUSED(argp);
    return OBOS_STATUS_INVALID_IOCTL;
}
void driver_cleanup_callback()
{}
OBOS_WEAK void on_wake();
OBOS_WEAK void on_suspend();

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES|DRIVER_HEADER_FLAGS_DETECT_VIA_PCI|DRIVER_HEADER_HAS_VERSION_FIELD,
    .acpiId.nPnpIds = 0,
    .pciId.indiv = {
        .classCode = 0x0c, // serial bus controller
        .subClass  = 0x03, // USB Controller
        .progIf    = 0x20, // EHCI
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
        .submit_irp = submit_irp,
        .finalize_irp = finalize_irp,
    },
    .driverName = "EHCI Driver",
    .version=1,
    .uacpi_init_level_required = PCI_IRQ_UACPI_INIT_LEVEL
};

driver_id* this_driver;

static void search_bus(pci_bus* bus)
{
    for (pci_device* dev = LIST_GET_HEAD(pci_device_list, &bus->devices); dev; )
    {
        if (dev->hid.indiv.classCode == drv_hdr.pciId.indiv.classCode &&
            dev->hid.indiv.subClass == drv_hdr.pciId.indiv.subClass &&
            dev->hid.indiv.progIf == drv_hdr.pciId.indiv.progIf)
        {
            g_controller_count++;
            g_controllers = Reallocate(OBOS_KernelAllocator, g_controllers, g_controller_count*sizeof(ehci_controller), (g_controller_count-1)*sizeof(ehci_controller), nullptr);
            memzero(&g_controllers[g_controller_count-1], sizeof(ehci_controller));
            g_controllers[g_controller_count-1].dev = dev;
        }

        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
    }
}

obos_status ehci_signal_connection_change(ehci_controller* controller, ehci_port* port, bool connected)
{
    OBOS_UNUSED(controller);
    OBOS_UNUSED(port);
    OBOS_UNUSED(connected);
    *port->sc |= BIT(1);
    return OBOS_STATUS_UNIMPLEMENTED;
}

driver_init_status OBOS_DriverEntry(driver_id* this)
{
    this_driver = this;
    for (size_t i = 0; i < Drv_PCIBusCount; i++)
        search_bus(&Drv_PCIBuses[i]);
    for (size_t i = 0; i < g_controller_count; i++)
        ehci_initialize_controller(&g_controllers[i]);
    return (driver_init_status){.status=OBOS_STATUS_SUCCESS};
}