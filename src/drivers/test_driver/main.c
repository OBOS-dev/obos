#include <int.h>
#include <klog.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <scheduler/thread.h>

static OBOS_PAGEABLE_VARIABLE driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = 0,
};

OBOS_PAGEABLE_FUNCTION DRV_EXPORT void thing()
{
    OBOS_Log("Test!\n");
}
extern char Drv_Base[];

OBOS_PAGEABLE_FUNCTION void OBOS_DriverEntry(driver_id* this)
{
    OBOS_UNUSED(this);
    OBOS_Log("%s: Hello from a test driver. Driver base: %p. Driver id: %d.\n", __func__, Drv_Base, this->id);
    thing();
    OBOS_Log("Exiting from main thread.\n");
    Core_ExitCurrentThread();
}