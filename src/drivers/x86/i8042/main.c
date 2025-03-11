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

#include "generic/libps2/keyboard.h"
#include "ps2_structs.h"
#include "uacpi/namespace.h"
#include "uacpi/types.h"

OBOS_WEAK obos_status get_blk_size(dev_desc desc, size_t* blkSize);
OBOS_WEAK obos_status get_max_blk_count(dev_desc desc, size_t* count);
OBOS_WEAK obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead);
OBOS_WEAK obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);
OBOS_WEAK obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata), void* userdata);  // unrequired for fs drivers.
OBOS_WEAK obos_status query_user_readable_name(dev_desc what, const char** name);
OBOS_WEAK obos_status ioctl(dev_desc what, uint32_t request, void* argp);
OBOS_WEAK void cleanup();

OBOS_WEAK void on_suspend();
OBOS_WEAK void on_wake();

const char* const pnp_ids[] = {
    "PNP0303",
    "PNP0F13",
    "PNP0F03",
    nullptr,
};

obos_status ioctl(dev_desc what, uint32_t request, void* argp);
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
        .driver_cleanup_callback = cleanup,
        .ioctl = ioctl,
        .get_blk_size = get_blk_size,
        .get_max_blk_count = get_max_blk_count,
        .query_user_readable_name = query_user_readable_name,
        .foreach_device = foreach_device,
        .read_sync = read_sync,
        .write_sync = write_sync,
        .on_suspend = on_suspend,
        .on_wake = on_wake,
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

    PS2_InitializeKeyboard(&PS2_CtlrData.ports[0]);

    return (driver_init_status){.status=OBOS_STATUS_SUCCESS,.fatal=false,.context=nullptr};
}
