/*
 * oboskrnl/power/suspend.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include "driver_interface/driverId.h"
#include <int.h>
#include <klog.h>
#include <error.h>

#include <power/suspend.h>
#include <power/device.h>

#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>
#include <scheduler/process.h>
#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>

#include <driver_interface/pci.h>

#include <locks/mutex.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <uacpi/sleep.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/event.h>
#include <uacpi/context.h>
#include <uacpi/namespace.h>
#include <uacpi/utilities.h>
#include <uacpi_arch_helpers.h>

#include <utils/list.h>

// Note: Only currently supports S3

static mutex suspend_lock = MUTEX_INITIALIZE();
// the thread that initiated the suspend.
static thread* suspended_thread = nullptr;
thread* OBOS_SuspendWorkerThread = nullptr;
bool OBOS_WokeFromSuspend;

static void restore_pci(pci_bus* bus)
{
    for (pci_device* dev = LIST_GET_HEAD(pci_device_list, &bus->devices); dev; )
    {
        // Restore the command register.
        Drv_PCISetResource(dev->resource_cmd_register);

        // Restore the other resources.
        for (pci_resource* resource = LIST_GET_HEAD(pci_device_list, &dev->resources); resource; )
        {
            if (resource->type == PCI_RESOURCE_CMD_REGISTER)
                goto next1;
            Drv_PCISetResource(resource);

            next1:
            resource = LIST_GET_NEXT(pci_resource_list, &dev->resources, resource);
        }

        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
    }
}

static void suspend_impl()
{
    if (OBOS_WokeFromSuspend)
    {
        // Call AML's wake functions.
        uacpi_prepare_for_wake_from_sleep_state(UACPI_SLEEP_STATE_S3);
        UACPI_ARCH_ENABLE_INTERRUPTS();
        uacpi_wake_from_sleep_state(UACPI_SLEEP_STATE_S3);

        // Restore PCI.
        for (size_t i = 0; i < Drv_PCIBusCount; i++)
            restore_pci(&Drv_PCIBuses[i]);

        // Tell all drivers we're awake.
        for (driver_node* node = Drv_LoadedDrivers.head; node; )
        {
            if (node->data->header.ftable.on_wake)
                node->data->header.ftable.on_wake();

            node = node->next;
        }

        // Wake the thread that suspended the kernel to begin with.
        OBOS_WokeFromSuspend = false;
        CoreH_ThreadReady(suspended_thread);
        Core_ExitCurrentThread();
    }
    // NOTE: It is up to the arch to unsuspend the scheduler.
    Core_SuspendScheduler(true);
    Core_WaitForSchedulerSuspend();
    uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S3);
    UACPI_ARCH_DISABLE_INTERRUPTS();
    // good night computer.
    uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S3);
    while(1)
        asm volatile("");
}
static uacpi_iteration_decision acpi_enumerate_callback(void *ctx, uacpi_namespace_node *node, uint32_t max_depth)
{
    OBOS_UNUSED(max_depth);

    obos_status status = OBOS_DeviceMakeWakeCapable(node, UACPI_SLEEP_STATE_S3, !!ctx);
    if (obos_is_error(status))
    {
        if (status != OBOS_STATUS_WAKE_INCAPABLE)
            OBOS_Warning("Could not make device wake capable. Status: %d. Continuing...\n", status);
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    return UACPI_ITERATION_DECISION_CONTINUE;
}
static void set_wake_devs()
{
    uacpi_namespace_for_each_child_simple(
        uacpi_namespace_root(),
        acpi_enumerate_callback,
        (void*)false
    );
}
void OBOS_InitWakeGPEs()
{
    uacpi_namespace_for_each_child_simple(
        uacpi_namespace_root(),
        acpi_enumerate_callback,
        (void*)true // only mark GPEs for wake.
    );
    uacpi_finalize_gpe_initialization();
}
obos_status OBOS_Suspend()
{
    if (uacpi_get_current_init_level() < UACPI_INIT_LEVEL_NAMESPACE_INITIALIZED)
        return OBOS_STATUS_INVALID_INIT_PHASE;
    if (obos_is_error(Core_MutexTryAcquire(&suspend_lock)))
        return OBOS_STATUS_ABORTED;
    uacpi_namespace_node* s3 = nullptr;
    uacpi_namespace_node_find(uacpi_namespace_root(), "_S3_", &s3);
    if (!s3)
    {
        OBOS_Error("Firmware does not have the _S3 sleep state\n");
        return OBOS_STATUS_UNIMPLEMENTED; // bios does NOT support suspend.
    }
    if (OBOSS_PrepareWakeVector)
    {
        obos_status status = OBOSS_PrepareWakeVector();
        if (obos_is_error(status))
            return status;
    }
    if (uacpi_unlikely_error(uacpi_set_waking_vector(OBOSS_WakeVector, 0)))
    {
        Core_MutexRelease(&suspend_lock);
        return OBOS_STATUS_INTERNAL_ERROR;
    }
    OBOS_Log("oboskrnl: Suspend requested\n");
    OBOS_Warning("Note: Framebuffer might die\n");

    // Set wake GPEs.
    set_wake_devs();

    // Tell all drivers we're going to sleep.
    for (driver_node* node = Drv_LoadedDrivers.head; node; )
    {
        if (node->data->header.ftable.on_suspend)
            node->data->header.ftable.on_suspend();

        node = node->next;
    }

    log_level old_log_level = OBOS_GetLogLevel();
    OBOS_SetLogLevel(LOG_LEVEL_NONE);
    uacpi_context_set_log_level(UACPI_LOG_ERROR);
    thread* thr = CoreH_ThreadAllocate(nullptr);
    thread_ctx ctx = {};
    CoreS_SetupThreadContext(&ctx, (uintptr_t)suspend_impl, 0, false, Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x10000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr), 0x10000);
    thr->stackFreeUserdata = &Mm_KernelContext;
    thr->stackFree = CoreH_VMAStackFree;
    thread_affinity bsp_affinity = 0;
    for (size_t i = 0; i < Core_CpuCount && !bsp_affinity; i++)
        if (Core_CpuInfo[i].isBSP)
            bsp_affinity = CoreH_CPUIdToAffinity(Core_CpuInfo[i].id);
    if (!bsp_affinity)
        bsp_affinity = 0b1; // assume cpu 0
    CoreS_SetThreadIRQL(&ctx, IRQL_DISPATCH);
    CoreH_ThreadInitialize(thr, THREAD_PRIORITY_URGENT, bsp_affinity, &ctx);
    OBOS_SuspendWorkerThread = thr;
    CoreH_ThreadReady(thr);
    suspended_thread = Core_GetCurrentThread();
    // We will be blocked until further notice.
    CoreH_ThreadBlock(suspended_thread, true);
    OBOS_SetLogLevel(old_log_level);
    Core_MutexRelease(&suspend_lock);
    OBOS_Log("oboskrnl: Woke up from suspend.\n");
    return OBOS_STATUS_SUCCESS;
}
