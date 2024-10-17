/*
 * oboskrnl/syscall.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <syscall.h>
#include <handle.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>

#include <scheduler/sched_sys.h>

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
    OBOS_Log("oboskrnl: Shutdown requested.\n");
    // We're at IRQL_DISPATCH which should probably be enough for prepare for sleep state.
    uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
    UACPI_ARCH_DISABLE_INTERRUPTS();
    uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    while(1)
        asm volatile("");
}
#else
void Sys_Reboot()
{
    OBOS_Log("Unimplemented: Sys_Reboot\n");
    return;
}
void Sys_Shutdown()
{
    OBOS_Log("Unimplemented: Sys_Shutdown\n");
    return;
}
#endif
uintptr_t OBOS_SyscallTable[SYSCALL_END-SYSCALL_BEGIN] = {
    (uintptr_t)Core_ExitCurrentThread,
    (uintptr_t)Core_Yield,
    (uintptr_t)Sys_Reboot,
    (uintptr_t)Sys_Shutdown,
    (uintptr_t)Sys_HandleClose,
    (uintptr_t)Sys_HandleClone,
    (uintptr_t)Sys_ThreadContextCreate, // 6
    (uintptr_t)Sys_ThreadContextRead,
    (uintptr_t)Sys_ThreadOpen,
    (uintptr_t)Sys_ThreadCreate,
    (uintptr_t)Sys_ThreadReady,
    (uintptr_t)Sys_ThreadBlock,
    (uintptr_t)Sys_ThreadBoostPriority,
    (uintptr_t)Sys_ThreadPriority,
    (uintptr_t)Sys_ThreadAffinity,
    (uintptr_t)Sys_ThreadSetOwner,
    (uintptr_t)Sys_ThreadGetTid, // 16
    (uintptr_t)Sys_WaitOnObject,
    (uintptr_t)Sys_WaitOnObjects,
    (uintptr_t)Sys_ProcessOpen,  // Unimplemented
    (uintptr_t)Sys_ProcessStart,
    (uintptr_t)Sys_ProcessKill,  // Unimplemented
};
uintptr_t OBOS_ArchSyscallTable[ARCH_SYSCALL_END-ARCH_SYSCALL_BEGIN] = {
};