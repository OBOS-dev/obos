/*
 * oboskrnl/arch/x86_64/syscall.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <cmdline.h>
#include <syscall.h>
#include <signal.h>
#include <handle.h>

#include <scheduler/schedule.h>
#include <scheduler/cpu_local.h>

#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/sse.h>
#include <arch/x86_64/cmos.h>
#include <arch/x86_64/lapic.h>

#include <mm/context.h>

#include <allocators/base.h>

#include <scheduler/sched_sys.h>
#include <scheduler/process.h>

#define IA32_EFER  0xC0000080
#define IA32_STAR  0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_CSTAR 0xC0000083
#define IA32_FSTAR 0xC0000084

extern uint64_t Arch_cpu_local_currentKernelStack_offset;
extern char Arch_SyscallTrapHandler[];
void OBOSS_InitializeSyscallInterface()
{
    // Enable IA32_EFER.SCE
    // This is done in CPU initialization
    // wrmsr(IA32_EFER, rdmsr(IA32_EFER) | BIT(0) /* SCE */);
    wrmsr(IA32_STAR, 0x0013000800000000); //  CS: 0x08, SS: 0x10, User CS: 0x1b, User SS: 0x23
    wrmsr(IA32_FSTAR, 0x43700); // Clear IF,TF,AC, and DF
    wrmsr(IA32_LSTAR, (uintptr_t)Arch_SyscallTrapHandler);
    Arch_cpu_local_currentKernelStack_offset = offsetof(cpu_local, currentKernelStack);
}

void SysS_SetFSBase(uintptr_t to)
{
    wrmsr(0xC0000100, to);
    Core_GetCurrentThread()->context.fs_base = to;
}

// Creates a new thread context, but takes in a stack pointer instead of a stack and stack size
handle SysS_ThreadContextCreateFork(uintptr_t entry, uintptr_t stack_pointer, handle vmm_context)
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

    ctx->ctx->cr3 = vmm_ctx->pt;
    ctx->ctx->frame.cr3 = vmm_ctx->pt;
    ctx->ctx->frame.rip = entry;
    ctx->ctx->frame.rsp = stack_pointer;
    ctx->ctx->frame.ss = 0x1b;
    ctx->ctx->frame.cs = 0x23;
    ctx->ctx->frame.rflags = 0x200202;
    ctx->ctx->fs_base = rdmsr(0xC0000100);
    ctx->ctx->extended_ctx_ptr = Arch_AllocateXSAVERegion();
    ctx->vmm_ctx = vmm_ctx;
    ctx->ctx->stackBase = Core_GetCurrentThread()->context.stackBase;
    ctx->ctx->stackSize = Core_GetCurrentThread()->context.stackSize;

    return hnd;
}

uintptr_t OBOS_ArchSyscallTable[ARCH_SYSCALL_END-ARCH_SYSCALL_BEGIN] = {
    (uintptr_t)SysS_SetFSBase,
    (uintptr_t)SysS_ThreadContextCreateFork,
    (uintptr_t)SysS_ClockGet,
    (uintptr_t)SysS_GDBStubBindInet,
    (uintptr_t)SysS_GDBStubBindDevice,
    (uintptr_t)SysS_GDBStubStart,
};

const char* syscall_to_string[] = {
    "Core_ExitCurrentThread/Sys_SetFSBase",
    "Core_Yield/Sys_ThreadContextCreateFork",
    "OBOS_Reboot/SysS_ClockGet",
    "OBOS_Shutdown/SysS_GDBStubBindInet",
    "Sys_HandleClose/SysS_GDBStubBindDevice",
    "Sys_HandleClone/SysS_GDBStubStart",
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
    "OBOS_STATUS_PORT_IN_USE",
	"OBOS_STATUS_ADDRESS_IN_USE",
    "OBOS_STATUS_ADDRESS_NOT_AVAILABLE",
	"OBOS_STATUS_NO_ROUTE_TO_HOST",
	"OBOS_STATUS_MESSAGE_TOO_BIG",
	"OBOS_STATUS_CONNECTION_REFUSED",
};

void Arch_LogSyscall(uintptr_t rdi, uintptr_t rsi, uintptr_t rdx, uintptr_t r8, uintptr_t r9, uint32_t eax)
{
    OBOS_UNUSED(rdi);
    OBOS_UNUSED(rsi);
    OBOS_UNUSED(rdx);
    OBOS_UNUSED(r8);
    OBOS_UNUSED(r9);
    OBOS_UNUSED(eax);
    if (eax >= sizeof(syscall_to_string)/sizeof(const char*) || !syscall_to_string[eax])
    {
        OBOS_Warning("(thread %ld, process %ld) invalid syscall %d(0x%p, 0x%p, 0x%p, 0x%p, 0x%p)\n", Core_GetCurrentThread()->tid, Core_GetCurrentThread()->proc->pid, eax, rdi,rsi,rdx,r8,r9);
        return;
    }
    OBOS_Debug("(thread %ld, process %ld) syscall %s(0x%p, 0x%p, 0x%p, 0x%p, 0x%p)\n", Core_GetCurrentThread()->tid, Core_GetCurrentThread()->proc->pid, syscall_to_string[eax], rdi,rsi,rdx,r8,r9);
}
void Arch_LogSyscallRet(uint64_t ret, uint32_t eax)
{
    OBOS_UNUSED(ret);
    OBOS_UNUSED(eax);
    if (eax >= sizeof(syscall_to_string)/sizeof(const char*) || !syscall_to_string[eax])
        return;
    static bool cached_opt = false, opt = false;
    if (!cached_opt)
    {
        opt = OBOS_GetOPTF("disable-syscall-error-log");
        cached_opt = true;
    }
    if (opt || 
        (ret == 0 || eax == 22 || eax == 42 || eax == 58 || eax == 34 || eax == 0
         || eax == 20 || eax == 59 || eax == 61 || eax == 9 || eax == 1 || eax == 19
         || eax == 2 || eax == 54 || (eax == 91 || ret == OBOS_STATUS_NOT_A_TTY)))
        OBOS_Debug("(thread %ld, process %ld) syscall %s returned 0x%x (%s)\n", Core_GetCurrentThread()->tid, Core_GetCurrentThread()->proc->pid, syscall_to_string[eax], ret, (ret < sizeof(status_to_string)/sizeof(status_to_string[0])) ? status_to_string[ret] : "no status string");
    else
        OBOS_Log("(thread %ld, process %ld) syscall %s returned 0x%x (%s)\n", Core_GetCurrentThread()->tid, Core_GetCurrentThread()->proc->pid, syscall_to_string[eax], ret, (ret < sizeof(status_to_string)/sizeof(status_to_string[0])) ? status_to_string[ret] : "no status string");
    if (Core_GetCurrentThread()->signal_info->pending)
        Arch_LAPICSendIPI(
            (ipi_lapic_info){.isShorthand=true,.info={.shorthand=LAPIC_DESTINATION_SHORTHAND_SELF}},
            (ipi_vector_info){.deliveryMode=LAPIC_DELIVERY_MODE_FIXED,.info.vector=Core_SchedulerIRQ->vector->id+0x20}
        );
}
