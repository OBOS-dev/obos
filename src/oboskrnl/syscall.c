/*
 * oboskrnl/syscall.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <syscall.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>

/*void Sys_Yield()
{
    OBOS_Debug("yielding\n");
    Core_Yield();
}*/
#if OBOS_ARCHITECTURE_HAS_ACPI
#include <uacpi/sleep.h>
void Sys_Reboot()
{
    uacpi_reboot();
    while(1)
        asm volatile("");
}
void Sys_Shutdown()
{
    // We're at IRQL_DISPATCH which should probably be enough for prepare for sleep state.
    uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
    UACPI_ARCH_DISABLE_INTERRUPTS();
    uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    while(1)
        asm volatile("");
}
#else
#endif
uintptr_t OBOS_SyscallTable[SYSCALL_END-SYSCALL_BEGIN] = {
    (uintptr_t)Core_ExitCurrentThread,
    (uintptr_t)Core_Yield,
    (uintptr_t)Sys_Reboot,
    (uintptr_t)Sys_Shutdown,
};
uintptr_t OBOS_ArchSyscallTable[ARCH_SYSCALL_END-ARCH_SYSCALL_BEGIN] = {
};
