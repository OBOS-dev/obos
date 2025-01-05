/*
 * oboskrnl/syscall.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <syscall.h>
#include <memmanip.h>
#include <handle.h>
#include <partition.h>
#include <signal.h>
#include <execve.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>
#include <scheduler/sched_sys.h>
#include <scheduler/process.h>

#include <power/shutdown.h>
#include <power/suspend.h>

#include <mm/mm_sys.h>
#include <mm/context.h>
#include <mm/fork.h>

#include <locks/sys_futex.h>

#include <vfs/fd_sys.h>

// TODO: Check permissions?
obos_status Sys_PartProbeDrive(handle ent, bool check_checksum)
{
    obos_status status = OBOS_STATUS_SUCCESS;

    handle_desc* dent = OBOS_HandleLookup(OBOS_CurrentHandleTable(), ent, HANDLE_TYPE_DIRENT, false, &status);
    if (!dent)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return OBOS_PartProbeDrive(dent->un.dirent, check_checksum);
}

obos_status Sys_InvalidSyscall()
{
    return OBOS_STATUS_NO_SYSCALL;
}

void Sys_LibCLog(const char* ustr)
{
    size_t str_len = 0;
    obos_status status = OBOSH_ReadUserString(ustr, nullptr, &str_len);
    if (obos_is_error(status))
    {
        OBOS_Error("libc wanted to log, but we got status %d trying to read the message.\n", status);
        return;
    }
    char* buf = OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, str_len+1, nullptr);
    OBOSH_ReadUserString(ustr, buf, &str_len);
    OBOS_LibCLog("%s\n", buf);
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, buf, str_len+1);
}

static handle Sys_MmFork()
{
    // Zeroed in Mm_ConstructContext
    context* ctx = Mm_Allocator->Allocate(Mm_Allocator, sizeof(context), nullptr);
    Mm_ConstructContext(ctx);
    Mm_ForkContext(ctx, Core_GetCurrentThread()->proc->ctx);
    ctx->workingSet.capacity = Core_GetCurrentThread()->proc->ctx->workingSet.capacity;

    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle_desc* desc = nullptr;
    handle hnd = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_VMM_CONTEXT, &desc);
    desc->un.vmm_context = ctx;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return hnd;

}

void Sys_ExitCurrentProcess(uint32_t exitCode)
{
    Core_ExitCurrentProcess((exitCode & 0xff) << 8);
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
    (uintptr_t)Sys_KillProcess,  // signal-related
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
    (uintptr_t)Sys_FdAlloc, // 34
    (uintptr_t)Sys_FdOpen,
    (uintptr_t)Sys_FdOpenDirent,
    (uintptr_t)Sys_FdWrite,
    (uintptr_t)Sys_FdAWrite,
    (uintptr_t)Sys_FdRead,
    (uintptr_t)Sys_FdARead,
    (uintptr_t)Sys_FdSeek,
    (uintptr_t)Sys_FdTellOff,
    (uintptr_t)Sys_FdEOF,
    (uintptr_t)Sys_FdIoctl,
    (uintptr_t)Sys_FdFlush, // 45
    (uintptr_t)OBOS_PartProbeAllDrives,
    (uintptr_t)Sys_PartProbeDrive, // 47
    (uintptr_t)OBOSS_SigReturn,
    (uintptr_t)Sys_Kill,
    (uintptr_t)Sys_SigAction,
    (uintptr_t)OBOS_SigSuspend,
    (uintptr_t)Sys_SigProcMask,
    (uintptr_t)Sys_SigAltStack, // 53
    (uintptr_t)Sys_OpenDir,
    (uintptr_t)Sys_ReadEntries, // 55
    (uintptr_t)Sys_ExecVE, // 56
    (uintptr_t)Sys_LibCLog, // 57
    (uintptr_t)Sys_ProcessGetPID, // 58
    (uintptr_t)Sys_ProcessGetPPID, // 59
    (uintptr_t)Sys_FdOpenAt,
    (uintptr_t)Sys_MmFork,
    (uintptr_t)Sys_ExitCurrentProcess,
    (uintptr_t)Sys_ProcessGetStatus,
    (uintptr_t)Sys_WaitProcess,
};
// Arch syscall table is defined per-arch

#undef OBOS_CROSSES_PAGE_BOUNDARY

bool OBOS_CROSSES_PAGE_BOUNDARY(void* ptr_, size_t sz)
{
    uintptr_t ptr = (uintptr_t)ptr_;
    uintptr_t limit = ptr+sz;
    limit -= (limit % OBOS_PAGE_SIZE);
    ptr -= (ptr % OBOS_PAGE_SIZE);
    return ptr != limit;
}

obos_status OBOSH_ReadUserString(const char* ustr, char* buf, size_t* sz_buf)
{
    if (buf && sz_buf)
        return memcpy_usr_to_k(buf, ustr, *sz_buf);

    context* ctx = CoreS_GetCPULocalPtr()->currentContext;

    obos_status status = OBOS_STATUS_SUCCESS;
    char* kstr = Mm_MapViewOfUserMemory(ctx, (void*)ustr, nullptr, OBOS_PAGE_SIZE, OBOS_PROTECTION_READ_ONLY, true, &status);
    if (!kstr)
        return status;

    char* iter = kstr;
    uintptr_t offset = (uintptr_t)kstr-(uintptr_t)iter;
    size_t currSize = OBOS_PAGE_SIZE;

    size_t str_len = 0;
    while ((*iter++) != 0)
    {
        if (OBOS_CROSSES_PAGE_BOUNDARY(iter, sizeof(*iter)*2))
        {
            Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)kstr, currSize);
            currSize += OBOS_PAGE_SIZE;
            kstr = Mm_MapViewOfUserMemory(ctx, (void*)(iter+1), nullptr, currSize, OBOS_PROTECTION_READ_ONLY, true, &status);
            if (!kstr)
                return status;
            iter = (char*)((uintptr_t)kstr + offset);
        }
        offset += 1;
        str_len++;
    }

    if (buf)
        memcpy(buf, kstr, OBOS_MIN(sz_buf ? *sz_buf : SIZE_MAX, str_len));
    if (sz_buf)
        *sz_buf = str_len;

    return OBOS_STATUS_SUCCESS;
}
