/*
 * oboskrnl/power/shutdown.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <struct_packing.h>

#include <irq/irql.h>

#include <scheduler/schedule.h>

#include <power/shutdown.h>

#include <uacpi/sleep.h>
#include <uacpi_arch_helpers.h>

OBOS_NORETURN void OBOS_Shutdown()
{
    Core_SuspendScheduler(true);
    Core_WaitForSchedulerSuspend();
    OBOS_Log("oboskrnl: Shutdown requested.\n");
    // We're at IRQL_DISPATCH which should probably be enough for prepare for sleep state.
    uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
    UACPI_ARCH_DISABLE_INTERRUPTS();
    uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    while(1)
        asm volatile("");
}
OBOS_NORETURN void OBOS_Reboot()
{
    Core_SuspendScheduler(true);
    Core_WaitForSchedulerSuspend();
    uacpi_reboot();
#ifdef __x86_64__
    UACPI_ARCH_DISABLE_INTERRUPTS();
    // Try triple faulting.
    struct {
        uint16_t limit;
        uint64_t base;
    } OBOS_PACK gdtr = {.limit=0x18-1};
    asm volatile ("lgdt (%0); mov $0x8, %%ax; mov %%ax, %%ss; push $0;" : :"r"(&gdtr) :"memory");
    // We should not be here anymore.
    OBOS_UNREACHABLE;
#endif
    while(1)
        asm volatile("");
}
