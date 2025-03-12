/*
 * drivers/x86/i8042/main.c
 * 
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <stdarg.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <scheduler/thread.h>

#include <irq/irq.h>
#include <irq/irql.h>

#include <locks/spinlock.h>

#include <vfs/vnode.h>
#include <vfs/dirent.h>

#include <uacpi/utilities.h>

#include <generic/libps2/detect.h>
#include <generic/libps2/controller.h>

#include "ps2_structs.h"

#include <uacpi/namespace.h>
#include <uacpi/types.h>

const char* const pnp_ids[] = {
    "PNP0303",
    "PNP0F13",
    "PNP0F03",
    nullptr,
};

OBOS_WEAK void cleanup()
{
    
}

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_VERSION_FIELD|DRIVER_HEADER_FLAGS_DETECT_VIA_ACPI,
    .acpiId = {
        .nPnpIds = 3,
        .pnpIds = {
            "PNP0303",
            "PNP0F13",
            "PNP0F03",
        }
    },
    .ftable = {
        .driver_cleanup_callback = cleanup
    },
    .driverName = "PS/2 Controller Driver",
    .version = 1,
    .uacpi_init_level_required = UACPI_INIT_LEVEL_NAMESPACE_LOADED
};

driver_id* this_driver;

static bool found_ps2_device;

static uacpi_iteration_decision match (
    void *user, uacpi_namespace_node *node, uacpi_u32 node_depth
)
{
    OBOS_UNUSED(user && node && node_depth);
    found_ps2_device = true;
    return UACPI_ITERATION_DECISION_BREAK;
}

OBOS_PAGEABLE_FUNCTION driver_init_status OBOS_DriverEntry(driver_id* this)
{
    this_driver = this;

    uacpi_find_devices_at(
        uacpi_namespace_root(),
        pnp_ids,
        match, nullptr   
    );
    if (!found_ps2_device)
        return (driver_init_status){.status=OBOS_STATUS_NOT_FOUND,.fatal=true,.context="Could not find a PS/2 Controller."};

    obos_status status = PS2_InitializeController();
    if (obos_is_error(status))
        return (driver_init_status){.status=status,.fatal=true,.context="Could not initialize the PS/2 Controller."};

    return (driver_init_status){.status=OBOS_STATUS_SUCCESS,.fatal=false,.context=nullptr};
}
