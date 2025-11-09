/*
 * oboskrnl/arch/m68k/syscall.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <syscall.h>
#include <handle.h>
#include <font.h>
#include <cmdline.h>

#include <arch/m68k/interrupt_frame.h>

#include <mm/context.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>
#include <scheduler/thread_context_info.h>
#include <scheduler/sched_sys.h>

#include <allocators/base.h>

const char *syscall_to_string[] = {
    "Core_ExitCurrentThread/Sys_SetTCB",
    "Core_Yield/Sys_GetTCB",
    "OBOS_Reboot/Sys_ThreadContextCreateFork",
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
    "Sys_Socket",
    "Sys_SendTo",
    "Sys_RecvFrom",
    "Sys_Listen",
    "Sys_Accept",
    "Sys_Bind",
    "Sys_Connect",
    "Sys_SockName",
    "Sys_PeerName",
    "Sys_GetSockOpt",
    "Sys_SetSockOpt",
    "Sys_ShutdownSocket",
    "Sys_GetHostname",
    "Sys_SetHostname",
    [127]="Sys_KillProcessGroup",
    "Sys_SetProcessGroup",
    "Sys_GetProcessGroup",
    "Sys_LinkAt",
    "Sys_FChmodAt",
    "Sys_FChownAt",
    "Sys_UMask",
    "Sys_RenameAt",
    "Sys_UTimeNSAt",
    "Sys_ThreadGetStack",
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

void Sys_SetTCB(void* tcb)
{
    Core_GetCurrentThread()->context.tcb = tcb;
}
void *Sys_GetTCB()
{
    return Core_GetCurrentThread()->context.tcb;
}
handle Sys_ThreadContextCreateFork(uintptr_t entry, uintptr_t stack_pointer, handle vmm_context);

uintptr_t OBOS_ArchSyscallTable[ARCH_SYSCALL_END-ARCH_SYSCALL_BEGIN] = {
    (uintptr_t)Sys_SetTCB,
    (uintptr_t)Sys_GetTCB,
    (uintptr_t)Sys_ThreadContextCreateFork,
};

void Arch_LogSyscall(uintptr_t p1, uintptr_t p2, uintptr_t p3, uintptr_t p4, uintptr_t p5, uint32_t sysnum);
void Arch_LogSyscallRet(uint32_t ret, uint32_t sysnum);

handle Sys_ThreadContextCreateFork(uintptr_t entry, uintptr_t stack_pointer, handle vmm_context)
{
    context* vmm_ctx =
        HANDLE_TYPE(vmm_context) == HANDLE_TYPE_CURRENT ?
            Core_GetCurrentThread()->proc->ctx : nullptr;
    if (!vmm_ctx)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        obos_status status = OBOS_STATUS_SUCCESS;
        handle_desc* vmm_ctx_desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), vmm_context, HANDLE_TYPE_VMM_CONTEXT, false, &status);
        OBOS_UNUSED(status);
        if (!vmm_ctx_desc)
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return HANDLE_INVALID;
        }
        vmm_ctx = vmm_ctx_desc->un.vmm_context;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    }

    handle_desc* desc = nullptr;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle hnd = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_THREAD_CTX, &desc);
    thread_ctx_handle *ctx = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(thread_ctx_handle), nullptr);
    desc->un.thread_ctx = ctx;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    ctx->ctx = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(thread_ctx), nullptr);
    ctx->canFree = true;
    ctx->lock = PUSHLOCK_INITIALIZE();

    ctx->ctx->urp = vmm_ctx->pt;
    ctx->ctx->pc = entry;
    ctx->ctx->usp = stack_pointer;
    ctx->ctx->sr = 0;
    ctx->ctx->tcb = Core_GetCurrentThread()->context.tcb;
    ctx->vmm_ctx = vmm_ctx;
    ctx->ctx->stackBase = Core_GetCurrentThread()->context.stackBase;
    ctx->ctx->stackSize = Core_GetCurrentThread()->context.stackSize;

    return hnd;
}

// parameters 1-5 are in d0-d4, respectively
// syscall number is in d5
// return value is in d0
void Arch_SyscallTrapHandler(interrupt_frame* frame)
{
    uint32_t syscall_number = frame->d5;
    uintptr_t* table = IS_ARCH_SYSCALL(syscall_number) ? OBOS_ArchSyscallTable : OBOS_SyscallTable;
    if (IS_ARCH_SYSCALL(syscall_number))
        syscall_number -= ARCH_SYSCALL_BEGIN;
    if (frame->d5 != (ARCH_SYSCALL_BEGIN+1) /* Sys_GetTCB */)
        Arch_LogSyscall(frame->d0, frame->d1, frame->d2, frame->d3, frame->d4, syscall_number);
    if (syscall_number >= SYSCALL_END)
    {
        frame->a1 = OBOS_STATUS_UNIMPLEMENTED;
        return;
    }
    uintptr_t(*hnd)(uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4, uint32_t p5) = (void*)table[syscall_number];
    if (hnd)
        frame->d0 = hnd(frame->d0, frame->d1, frame->d2, frame->d3, frame->d4);
    else
        frame->d0 = OBOS_STATUS_UNIMPLEMENTED;
    if (frame->d5 != (ARCH_SYSCALL_BEGIN+1) /* Sys_GetTCB */)
        Arch_LogSyscallRet(frame->d0, syscall_number);
}

void OBOSS_InitializeSyscallInterface()
{
    Arch_RawRegisterInterrupt(32 /* trap 0 */, (uintptr_t)Arch_SyscallTrapHandler);
}

void Arch_LogSyscall(uintptr_t d0, uintptr_t d1, uintptr_t d2, uintptr_t d3, uintptr_t d4, uint32_t sysnum)
{
    if (sysnum >= sizeof(syscall_to_string)/sizeof(const char*) || !syscall_to_string[sysnum])
    {
        OBOS_Warning("(thread %d, process %d) invalid syscall %d(0x%p, 0x%p, 0x%p, 0x%p, 0x%p)\n", (uint32_t)Core_GetCurrentThread()->tid, Core_GetCurrentThread()->proc->pid, sysnum, d0,d1,d2,d3,d4);
        return;
    }
    OBOS_Debug("(thread %d, process %d) syscall %s(0x%p, 0x%p, 0x%p, 0x%p, 0x%p)\n", (uint32_t)Core_GetCurrentThread()->tid, Core_GetCurrentThread()->proc->pid, syscall_to_string[sysnum], d0,d1,d2,d3,d4);
}
void Arch_LogSyscallRet(uint32_t ret, uint32_t sysnum)
{
    OBOS_UNUSED(ret);
    OBOS_UNUSED(sysnum);
    if (sysnum >= sizeof(syscall_to_string)/sizeof(const char*) || !syscall_to_string[sysnum])
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
        OBOS_Debug("(thread %d, process %d) syscall %s returned 0x%x (%s)\n", (uint32_t)Core_GetCurrentThread()->tid, Core_GetCurrentThread()->proc->pid, syscall_to_string[sysnum], ret, (ret < sizeof(status_to_string)/sizeof(status_to_string[0])) ? status_to_string[ret] : "no status string");
    else
        OBOS_Log("(thread %d, process %d) syscall %s returned 0x%x (%s)\n", (uint32_t)Core_GetCurrentThread()->tid, Core_GetCurrentThread()->proc->pid, syscall_to_string[sysnum], ret, (ret < sizeof(status_to_string)/sizeof(status_to_string[0])) ? status_to_string[ret] : "no status string");

}