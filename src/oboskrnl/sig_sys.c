/*
 * oboskrnl/sig_sys.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <handle.h>
#include <memmanip.h>
#include <signal.h>

#include <locks/wait.h>

#include <scheduler/schedule.h>
#include <scheduler/process.h>

#define thread_object_from_handle(hnd, return_status, use_curr) \
({\
    struct thread* result__ = nullptr;\
    if (HANDLE_TYPE(hnd) == HANDLE_TYPE_CURRENT && use_curr)\
        result__ = Core_GetCurrentThread();\
        else {\
            OBOS_LockHandleTable(OBOS_CurrentHandleTable());\
            obos_status status = OBOS_STATUS_SUCCESS;\
            handle_desc* _thr = OBOS_HandleLookup(OBOS_CurrentHandleTable(), (hnd), HANDLE_TYPE_THREAD, false, &status);\
            if (obos_is_error(status))\
            {\
                OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());\
                return return_status ? status : HANDLE_INVALID;\
            }\
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());\
            result__ = _thr->un.thread;\
        }\
        result__;\
})

obos_status Sys_Kill(handle thr, int sigval)
{
    thread* target = thread_object_from_handle(thr, true, true);
    return OBOS_Kill(Core_GetCurrentThread(), target, sigval);
}
obos_status Sys_KillProcess(handle proc_hnd, int sigval)
{
    struct process* proc =
        HANDLE_TYPE(proc_hnd) == HANDLE_TYPE_CURRENT ?
            Core_GetCurrentThread()->proc :
            nullptr;
    if (!proc)
    {
        obos_status status = OBOS_STATUS_SUCCESS;
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), proc_hnd, HANDLE_TYPE_PROCESS, false, &status);
        if (!desc)
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return status;
        }
        proc = desc->un.process;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    }
    return OBOS_KillProcess(proc, sigval);
}

obos_status Sys_SigAction(int signum, const user_sigaction* act, user_sigaction* oldact)
{
    sigaction kact = {};
    obos_status status = OBOS_STATUS_SUCCESS;
    if (act)
        status = memcpy_usr_to_k(&kact, act, sizeof(user_sigaction));
    if (obos_is_error(status))
        return status;

    sigaction koldact = {};
    status = OBOS_SigAction(signum, act ? &kact : nullptr, oldact ? &koldact : nullptr);
    if (obos_is_error(status))
        return status;

    if (oldact)
        status = memcpy_k_to_usr(oldact, &koldact, sizeof(user_sigaction));

    return status;
}

obos_status Sys_SigPending(sigset_t* mask)
{
    if (!mask)
        return OBOS_STATUS_INVALID_ARGUMENT;
    sigset_t kmask = 0;
    obos_status status = OBOS_SigPending(&kmask);
    if (obos_is_error(status))
        return status;
    return memcpy_k_to_usr(mask, &kmask, sizeof(sigset_t));
}

obos_status Sys_SigProcMask(int how, const sigset_t* mask, sigset_t* oldset)
{
    sigset_t kmask = {};
    obos_status status = OBOS_STATUS_SUCCESS;
    if (mask)
        status = memcpy_usr_to_k(&kmask, mask, sizeof(sigset_t));

    if (obos_is_error(status)) return status;

    sigset_t koldset = {};
    status = OBOS_SigProcMask(how, mask ? &kmask : nullptr, oldset ? &koldset : nullptr);
    if (obos_is_error(status))
        return status;

    if (oldset)
        status = memcpy_k_to_usr(oldset, &koldset, sizeof(sigset_t));

    return status;
}

obos_status Sys_SigAltStack(const stack_t* sp, stack_t* oldsp)
{
    stack_t koldsp = {};
    koldsp.ss_size = Core_GetCurrentThread()->signal_info->stack_size;
#if defined(__x86_64__) || defined(__m68k__) || defined(__i386__)
    koldsp.ss_sp = (void*)(Core_GetCurrentThread()->signal_info->sp - Core_GetCurrentThread()->signal_info->stack_size);
#else
#   error Sys_SigAltStack needs some arch-specific code which is unimplemented, this is a bug, report it.
#endif
    koldsp.ss_flags = 0;

    obos_status status = OBOS_STATUS_SUCCESS;
    if (oldsp)
        status = memcpy_k_to_usr(&oldsp, &koldsp, sizeof(stack_t));
    if (obos_is_error(status))
        return status;

    if (sp)
    {
        stack_t ksp = {};
        status = memcpy_usr_to_k(&ksp, sp, sizeof(ksp));
        if (obos_is_error(status))
            return status;

        Core_MutexAcquire(&Core_GetCurrentThread()->signal_info->lock);

        if (ksp.ss_flags & SS_DISABLE)
        {
            Core_GetCurrentThread()->signal_info->sp = 0;
            Core_GetCurrentThread()->signal_info->stack_size = 0;
            goto done;
        }

#if defined(__x86_64__) || defined(__m68k__) || defined(__i386__)
        Core_GetCurrentThread()->signal_info->sp = ((uintptr_t)ksp.ss_sp) + ksp.ss_size;
#else
#   error Sys_SigAltStack needs some arch-specific code which is unimplemented, this is a bug, report it.
#endif
        Core_GetCurrentThread()->signal_info->stack_size = ksp.ss_size;

        done:
        Core_MutexRelease(&Core_GetCurrentThread()->signal_info->lock);
    }

    return OBOS_STATUS_SUCCESS;
}
