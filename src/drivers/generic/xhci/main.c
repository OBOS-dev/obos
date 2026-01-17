/*
 * drivers/generic/xhci/main.c
 *
 * Copyright (c) 2026 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <driver_interface/header.h>
#include <driver_interface/pci.h>
#include <driver_interface/driverId.h>

#include "xhci.h"

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

driver_id* this_driver;

OBOS_WEAK void on_wake();
OBOS_WEAK void on_suspend();

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_FLAGS_DETECT_VIA_PCI|DRIVER_HEADER_HAS_VERSION_FIELD,
    .acpiId.nPnpIds = 0,
    .pciId.indiv = {
        .classCode = 0x0C, // Serial Controller
        .subClass  = 0x03, // USB Controller
        .progIf    = 0x30, // USB3 (XHCI)
    },
    .ftable = {
        .driver_cleanup_callback = driver_cleanup_callback,
        .ioctl = ioctl,
        .on_wake = on_wake,
        .on_suspend = on_suspend,
        .submit_irp = submit_irp,
        .finalize_irp = finalize_irp,
    },
    .driverName = "XHCI Driver",
    .version=1,
    .uacpi_init_level_required = PCI_IRQ_UACPI_INIT_LEVEL
};

driver_init_status OBOS_DriverEntry(driver_id* this)
{
    this_driver = this;
    for (size_t i = 0; i < Drv_PCIBusCount; i++)
        xhci_probe_bus(&Drv_PCIBuses[i]);
    return (driver_init_status){.status=OBOS_STATUS_SUCCESS,.fatal=false,.context=nullptr};
}