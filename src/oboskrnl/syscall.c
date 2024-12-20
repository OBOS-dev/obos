/*
 * oboskrnl/syscall.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <syscall.h>
#include <handle.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>

#include <scheduler/sched_sys.h>

#include <power/shutdown.h>
#include <power/suspend.h>

#include <mm/mm_sys.h>

#include <locks/sys_futex.h>

/*void Sys_Yield()
{
    OBOS_Debug("yielding\n");
    Core_Yield();
}*/
obos_status Sys_InvalidSyscall()
{
    return OBOS_STATUS_NO_SYSCALL;
}
uintptr_t OBOS_SyscallTable[SYSCALL_END-SYSCALL_BEGIN] = {
    (uintptr_t)Core_ExitCurrentThread,
    (uintptr_t)Core_Yield,
    (uintptr_t)OBOS_Reboot,
    (uintptr_t)OBOS_Shutdown,
    (uintptr_t)Sys_HandleClose,
    (uintptr_t)Sys_HandleClone,
    (uintptr_t)Sys_ThreadContextCreate, // 6
    (uintptr_t)OBOS_Suspend,
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
    (uintptr_t)Sys_VirtualMemoryAlloc, // 22
    (uintptr_t)Sys_VirtualMemoryFree,
    (uintptr_t)Sys_VirtualMemoryProtect,
    (uintptr_t)Sys_VirtualMemoryLock, // Unimplemented
    (uintptr_t)Sys_VirtualMemoryUnlock, // Unimplemented
    (uintptr_t)Sys_MakeNewContext,
    (uintptr_t)Sys_ContextExpandWSCapacity,
    (uintptr_t)Sys_ContextGetStat,
    (uintptr_t)Sys_GetUsedPhysicalMemoryCount,
    (uintptr_t)Sys_QueryPageInfo,
    (uintptr_t)Sys_FutexWake, // 32
    (uintptr_t)Sys_FutexWait,
};
// Arch syscall table is defined per-arch