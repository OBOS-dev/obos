/*
 * oboskrnl/arch/m68k/syscall.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <syscall.h>
#include <font.h>
#include <cmdline.h>

#include <arch/m68k/interrupt_frame.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>

const char *syscall_to_string[] = {
    "Core_ExitCurrentThread",
    "Core_Yield",
    "OBOS_Reboot",
    "OBOS_Shutdown",
    "Sys_HandleClose",
    "Sys_HandleClone",
    "Sys_ThreadContextCreate", // 6
    "OBOS_Suspend",
    "Sys_ThreadOpen",
    "Sys_ThreadCreate",
    "Sys_ThreadReady",
    "Sys_ThreadBlock",
    "Sys_ThreadBoostPriority",
    "Sys_ThreadPriority",
    "Sys_ThreadAffinity",
    "Sys_ThreadSetOwner",
    "Sys_ThreadGetTid", // 16
    "Sys_WaitOnObject",
    "Sys_Fcntl",
    "Sys_ProcessOpen",  // Unimplemented
    "Sys_ProcessStart",
    "Sys_KillProcess",  // signal-related
    "Sys_VirtualMemoryAlloc", // 22
    "Sys_VirtualMemoryFree",
    "Sys_VirtualMemoryProtect",
    "Sys_VirtualMemoryLock", // Unimplemented
    "Sys_VirtualMemoryUnlock", // Unimplemented
    "Sys_MakeNewContext",
    "Sys_ContextExpandWSCapacity",
    "Sys_ContextGetStat",
    "Sys_GetUsedPhysicalMemoryCount",
    "Sys_QueryPageInfo",
    "Sys_FutexWake", // 32
    "Sys_FutexWait",
    "Sys_FdAlloc", // 34
    "Sys_FdOpen",
    "Sys_FdOpenDirent",
    "Sys_FdWrite",
    "Sys_IRPCreate",
    "Sys_FdRead",
    "Sys_IRPWait",
    "Sys_FdSeek",
    "Sys_FdTellOff",
    "Sys_FdEOF",
    "Sys_FdIoctl",
    "Sys_FdFlush", // 45
    "OBOS_PartProbeAllDrives",
    "Sys_PartProbeDrive", // 47
    "OBOSS_SigReturn",
    "Sys_Kill",
    "Sys_SigAction",
    "OBOS_SigSuspend",
    "Sys_SigProcMask",
    "Sys_SigAltStack", // 53
    "Sys_OpenDir",
    "Sys_ReadEntries", // 55
    "Sys_ExecVE", // 56
    "Sys_LibCLog", // 57
    "Sys_ProcessGetPID", // 58
    "Sys_ProcessGetPPID", // 59
    "Sys_FdOpenAt",
    "Sys_MmFork",
    "Sys_ExitCurrentProcess",
    "Sys_ProcessGetStatus",
    "Sys_WaitProcess",
    "Sys_Stat",
    "Sys_StatFSInfo",
    "Sys_SysConf",
    "Sys_SetKLogLevel",
    "Sys_LoadDriver",
    "Sys_StartDriver",
    "Sys_UnloadDriver",
    "Sys_PnpLoadDriversAt",
    "Sys_FindDriverByName",
    "Sys_EnumerateLoadedDrivers",
    "Sys_QueryDriverName",
    "Sys_Sync",
    "Sys_SleepMS",
    "Sys_Mount",
    "Sys_Unmount",
    "Sys_FdCreat",
    "Sys_FdOpenEx",
    "Sys_FdOpenAtEx",
    "Sys_Mkdir",
    "Sys_MkdirAt",
    "Sys_Chdir",
    "Sys_ChdirEnt",
    "Sys_GetCWD",
    "Sys_SetControllingTTY",
    "Sys_GetControllingTTY",
    "Sys_TTYName",
    "Sys_IsATTY",
    "Sys_IRPWait",
    "Sys_IRPQueryState",
    "Sys_IRPGetBuffer",
    "Sys_IRPGetStatus",
    "Sys_CreatePipe",
    "Sys_PSelect",
    "Sys_ReadLinkAt",
    "Sys_SetUid",
    "Sys_SetGid",
    "Sys_GetUid",
    "Sys_GetGid",
    "Sys_UnlinkAt",
    "Sys_MakeDiskSwap",
    "Sys_SwitchSwap",
    "Sys_SyncAnonPages",
    "Sys_FdPWrite",
    "Sys_FdPRead",
    "Sys_SymLink",
    "Sys_SymLinkAt",
    "Sys_CreateNamedPipe",
    "Sys_PPoll",
};

const char* status_to_string[] = {
    "OBOS_STATUS_SUCCESS",
    "OBOS_STATUS_INVALID_IRQL",
    "OBOS_STATUS_INVALID_ARGUMENT",
    "OBOS_STATUS_UNIMPLEMENTED",
    "OBOS_STATUS_INVALID_INIT_PHASE",
    "OBOS_STATUS_INVALID_AFFINITY",
    "OBOS_STATUS_NOT_ENOUGH_MEMORY",
    "OBOS_STATUS_MISMATCH",
    "OBOS_STATUS_INTERNAL_ERROR",
    "OBOS_STATUS_RETRY",
    "OBOS_STATUS_ALREADY_INITIALIZED",
    "OBOS_STATUS_NOT_FOUND",
    "OBOS_STATUS_IN_USE",
    "OBOS_STATUS_ACCESS_DENIED",
    "OBOS_STATUS_UNINITIALIZED",
    "OBOS_STATUS_UNHANDLED",
    "OBOS_STATUS_UNPAGED_POOL",
    "OBOS_STATUS_INVALID_FILE",
    "OBOS_STATUS_INVALID_HEADER",
    "OBOS_STATUS_DRIVER_REFERENCED_UNRESOLVED_SYMBOL",
    "OBOS_STATUS_DRIVER_SYMBOL_MISMATCH",
    "OBOS_STATUS_NO_ENTRY_POINT",
    "OBOS_STATUS_INVALID_IOCTL",
    "OBOS_STATUS_INVALID_OPERATION",
    "OBOS_STATUS_DPC_ALREADY_ENQUEUED",
    "OBOS_STATUS_RECURSIVE_LOCK",
    "OBOS_STATUS_READ_ONLY",
    "OBOS_STATUS_NOT_A_FILE",
    "OBOS_STATUS_ALREADY_MOUNTED",
    "OBOS_STATUS_EOF",
    "OBOS_STATUS_ABORTED",
    "OBOS_STATUS_PAGE_FAULT",
    "OBOS_STATUS_TIMED_OUT",
    "OBOS_STATUS_PIPE_CLOSED",
    "OBOS_STATUS_NO_SPACE",
    "OBOS_STATUS_NO_SYSCALL",
    "OBOS_STATUS_WAKE_INCAPABLE",
    "OBOS_STATUS_INVALID_ELF_TYPE",
    "OBOS_STATUS_WOULD_BLOCK",
    "OBOS_STATUS_NOT_A_TTY",
    "OBOS_STATUS_IRP_RETRY",
};

void Arch_RawRegisterInterrupt(uint8_t vec, uintptr_t f);

uintptr_t OBOS_ArchSyscallTable[ARCH_SYSCALL_END-ARCH_SYSCALL_BEGIN];

void Arch_LogSyscall(uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4, uintptr_t p5, uint32_t sysnum);
void Arch_LogSyscallRet(uint32_t ret, uint32_t sysnum);

void Arch_SyscallTrapHandler(interrupt_frame* frame)
{
    uint32_t syscall_number = frame->d5;
    uintptr_t* table = IS_ARCH_SYSCALL(syscall_number) ? OBOS_ArchSyscallTable : OBOS_SyscallTable;
    syscall_number -= ARCH_SYSCALL_BEGIN;
    Arch_LogSyscall(frame->d0, frame->d1, frame->d2, frame->d3, frame->d5, syscall_number);
    if (syscall_number >= SYSCALL_END)
    {
        frame->a1 = OBOS_STATUS_UNIMPLEMENTED;
        return;
    }
    uintptr_t(*hnd)(uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4, uint32_t p5) = (void*)table[syscall_number];
    frame->a1 = hnd(frame->d0, frame->d1, frame->d2, frame->d3, frame->d5);
    Arch_LogSyscallRet(frame->a1, syscall_number);
}

void OBOSS_InitializeSyscallInterface()
{
    Arch_RawRegisterInterrupt(32 /* trap 0 */, (uintptr_t)Arch_SyscallTrapHandler);
}

void Arch_LogSyscall(uintptr_t d0, uintptr_t d1, uintptr_t d2, uintptr_t d3, uintptr_t d4, uint32_t sysnum)
{
    if (sysnum >= sizeof(syscall_to_string)/sizeof(const char*))
    {
        OBOS_Warning("(thread %ld, process %ld) invalid syscall %d(0x%p, 0x%p, 0x%p, 0x%p, 0x%p)\n", Core_GetCurrentThread()->tid, Core_GetCurrentThread()->proc->pid, sysnum, d0,d1,d2,d3,d4);
        return;
    }
    OBOS_Debug("(thread %ld, process %ld) syscall %s(0x%p, 0x%p, 0x%p, 0x%p, 0x%p)\n", Core_GetCurrentThread()->tid, Core_GetCurrentThread()->proc->pid, syscall_to_string[sysnum], d0,d1,d2,d3,d4);
}
void Arch_LogSyscallRet(uint32_t ret, uint32_t sysnum)
{
    OBOS_UNUSED(ret);
    OBOS_UNUSED(sysnum);
    if (sysnum >= sizeof(syscall_to_string)/sizeof(const char*))
        return;
    static bool cached_opt = false, opt = false;
    if (!cached_opt)
    {
        opt = OBOS_GetOPTF("disable-syscall-error-log");
        cached_opt = true;
    }
    if (opt || 
        (ret == 0 || sysnum == 22 || sysnum == 42 || sysnum == 58 || sysnum == 34 || sysnum == 0
         || sysnum == 20 || sysnum == 59 || sysnum == 61 || sysnum == 9 || sysnum == 1 || sysnum == 19
         || sysnum == 2 || (sysnum == 91 || ret == OBOS_STATUS_NOT_A_TTY)))
        OBOS_Debug("(thread %ld, process %ld) syscall %s returned 0x%x (%s)\n", Core_GetCurrentThread()->tid, Core_GetCurrentThread()->proc->pid, syscall_to_string[sysnum], ret, (ret < sizeof(status_to_string)/sizeof(status_to_string[0])) ? status_to_string[ret] : "no status string");
    else
        OBOS_Log("(thread %ld, process %ld) syscall %s returned 0x%x (%s)\n", Core_GetCurrentThread()->tid, Core_GetCurrentThread()->proc->pid, syscall_to_string[sysnum], ret, (ret < sizeof(status_to_string)/sizeof(status_to_string[0])) ? status_to_string[ret] : "no status string");

}