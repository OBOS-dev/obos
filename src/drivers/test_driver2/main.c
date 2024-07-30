#include <int.h>
#include <klog.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <scheduler/thread.h>

static OBOS_PAGEABLE_VARIABLE driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = 0,
};

extern char Drv_Base[];

// Located in test driver.
extern void TestDriver_Test(driver_id*);

OBOS_PAGEABLE_FUNCTION void OBOS_DriverEntry(driver_id* this)
{
    OBOS_Log("%s: Hello from test driver #2. Driver base: %p. Driver id: %d.\n", __func__, Drv_Base, this->id);
    TestDriver_Test(this);
    OBOS_Log("Exiting from main thread.\n");
    Core_ExitCurrentThread();
}