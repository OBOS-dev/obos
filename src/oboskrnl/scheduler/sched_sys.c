/*
 * oboskrnl/scheduler/sched_sys.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include "vfs/mount.h"
#include <int.h>
#include <error.h>
#include <signal.h>
#include <klog.h>
#include <handle.h>
#include <memmanip.h>

#include <scheduler/sched_sys.h>
#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>
#include <scheduler/process.h>
#include <scheduler/schedule.h>

#include <scheduler/cpu_local.h>
#include <allocators/base.h>

#include <mm/context.h>
#include <mm/alloc.h>

#include <locks/pushlock.h>
#include <locks/wait.h>

#include <vfs/fd_sys.h>

// scheduler/thread_context_info.h

handle Sys_ThreadContextCreate(uintptr_t entry, uintptr_t arg1, void* stack, size_t stack_size, handle vmm_context)
{
    if (!stack || !stack_size)
        return HANDLE_INVALID;

    context* vmm_ctx =
        HANDLE_TYPE(vmm_context) == HANDLE_TYPE_CURRENT ?
            Core_GetCurrentThread()->proc->ctx : nullptr;
    if (!vmm_ctx)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* vmm_ctx_desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), vmm_context, HANDLE_TYPE_VMM_CONTEXT, false, nullptr);
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
    ctx->vmm_ctx = vmm_ctx;

    CoreS_SetupThreadContext(ctx->ctx, entry, arg1, true, stack, stack_size);
    CoreS_SetThreadPageTable(ctx->ctx, vmm_ctx->pt);

    return hnd;
}
/*
 * We have removed this syscall since it simply leaks kernel data, and does not do what userspace would expect.
obos_status Sys_ThreadContextRead(handle thread_context, struct thread_context_info* out)
{
    if (!out)
        return OBOS_STATUS_INVALID_ARGUMENT;

    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* ctx = OBOS_HandleLookup(OBOS_CurrentHandleTable(), thread_context, HANDLE_TYPE_VMM_CONTEXT, false, &status);
    if (!ctx)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    thread_ctx_handle* thr_ctx = ctx->un.thread_ctx;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    Core_PushlockAcquire(&thr_ctx->lock, true);
    status = memcpy_k_to_usr(out, thr_ctx->ctx, sizeof(*out));
    Core_PushlockRelease(&thr_ctx->lock, true);
    return status;
}*/

// scheduler/thread.h

handle Sys_ThreadOpen(handle proc_hnd, uint64_t tid)
{
    if (!tid)
        return HANDLE_INVALID;
    if (tid <= (Core_CpuCount + 1))
        return HANDLE_INVALID; // you cannot open any CPU's idle thread

    process* parent = HANDLE_TYPE(proc_hnd) == HANDLE_TYPE_CURRENT ? Core_GetCurrentThread()->proc : nullptr;

    if (!parent)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* proc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), proc_hnd, HANDLE_TYPE_PROCESS, false, nullptr);
        if (!proc )
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return HANDLE_INVALID;
        }
        parent = proc->un.process;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    }

    thread* thr = nullptr;
    for (thread_node* curr = parent->threads.head; curr; )
    {
        if (curr->data->tid == tid)
        {
            thr = curr->data;
            break;
        }

        curr = curr->next;
    }

    if (!thr)
        return HANDLE_INVALID;

    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle_desc* desc = nullptr;
    handle hnd = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_THREAD, &desc);
    desc->un.thread = thr;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return hnd;
}
handle Sys_ThreadCreate(thread_priority priority, thread_affinity affinity, handle thread_context)
{
    if (priority < 0 || priority > THREAD_PRIORITY_MAX_VALUE)
        return HANDLE_INVALID;
    if (!affinity)
        affinity = Core_DefaultThreadAffinity;

    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* ctx = OBOS_HandleLookup(OBOS_CurrentHandleTable(), thread_context, HANDLE_TYPE_THREAD_CTX, false, &status);
    if (!ctx)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return HANDLE_INVALID;
    }
    if (!ctx->un.thread_ctx->canFree)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return HANDLE_INVALID;
    }

    thread* thr = CoreH_ThreadAllocate(nullptr);
    CoreH_ThreadInitialize(thr, priority, affinity, ctx->un.thread_ctx->ctx);
    thr->signal_info = OBOSH_AllocateSignalHeader();

    if (ctx->un.thread_ctx->canFree)
    {
        Core_PushlockAcquire(&ctx->un.thread_ctx->lock, false);
        ctx->un.thread_ctx->canFree = false;
        ctx->un.thread_ctx->ctx = &thr->context;
        Core_PushlockRelease(&ctx->un.thread_ctx->lock, false);
    }

    handle_desc* desc = nullptr;
    handle hnd = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_THREAD, &desc);
    desc->un.thread = thr;
    thr->references++;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    thr->kernelStack = Mm_AllocateKernelStack(ctx->un.thread_ctx->vmm_ctx, nullptr);

    return hnd;
}
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
obos_status Sys_ThreadReady(handle thread)
{
    struct thread* thr = thread_object_from_handle(thread, true, false);
    OBOS_ASSERT(thr);
    if (!thr->proc)
        return OBOS_STATUS_INVALID_INIT_PHASE;
    if (!thr->kernelStack)
        thr->kernelStack = Mm_AllocateKernelStack(thr->proc->ctx, nullptr);
    return CoreH_ThreadReady(thr);
}
obos_status Sys_ThreadBlock(handle thread)
{
    struct thread* thr = thread_object_from_handle(thread, true, true);
    OBOS_ASSERT(thr);
    return CoreH_ThreadBlock(thr, true);
}
obos_status Sys_ThreadBoostPriority(handle thread, int reserved /* ignored as of now */)
{
    OBOS_UNUSED(reserved);
    struct thread* thr = thread_object_from_handle(thread, true, true);
    OBOS_ASSERT(thr);
    return CoreH_ThreadBoostPriority(thr);
}
obos_status Sys_ThreadPriority(handle thread_hnd, const thread_priority *new, thread_priority* old)
{
    struct thread* thr = thread_object_from_handle(thread_hnd, true, true);
    OBOS_ASSERT(thr);
    if (old)
        memcpy_k_to_usr(old, &thr->priority, sizeof(thr->priority));
    if (new)
    {
        thread_priority old_priority = thr->priority;
        memcpy_usr_to_k(&thr->priority, new, sizeof(thr->priority));
        if (thr->priority != old_priority && thr->masterCPU) 
        {
            CoreH_ThreadListRemove(&thr->masterCPU->priorityLists[old_priority].list, thr->snode);
            CoreH_ThreadListAppend(&thr->masterCPU->priorityLists[thr->priority].list, thr->snode);
        }
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status Sys_ThreadAffinity(handle thread_hnd, const thread_affinity *new, thread_affinity* old)
{
    struct thread* thr = thread_object_from_handle(thread_hnd, true, true);
    OBOS_ASSERT(thr);
    if (old)
        memcpy_k_to_usr(old, &thr->affinity, sizeof(thr->affinity));
    if (new)
        memcpy_usr_to_k(&thr->affinity, new, sizeof(thr->affinity));
    return OBOS_STATUS_SUCCESS;
}
// Can only be called once per thread-object, and must be called before readying a thread.
obos_status Sys_ThreadSetOwner(handle thr_hnd, handle proc_hnd)
{
    struct thread* thr = thread_object_from_handle(thr_hnd, true, false);
    OBOS_ASSERT(thr);
    if (thr->proc)
        return OBOS_STATUS_ALREADY_INITIALIZED;

    struct process* proc =
        HANDLE_TYPE(proc_hnd) == HANDLE_TYPE_CURRENT ?
            Core_GetCurrentThread()->proc :
            nullptr;
    if (!proc)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        obos_status status = OBOS_STATUS_SUCCESS;
        handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), proc_hnd, HANDLE_TYPE_PROCESS, false, &status);
        if (!desc)
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return status;
        }
        proc = desc->un.process;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    }

    return Core_ProcessAppendThread(proc, thr);
}
uint64_t Sys_ThreadGetTid(handle thread_hnd)
{
    struct thread* thr = thread_object_from_handle(thread_hnd, true, false);
    OBOS_ASSERT(thr);
    return thr->tid;
}

// locks/wait.h

obos_status Sys_WaitOnObject(handle object /* must be a waitable handle */)
{
    handle_type type = HANDLE_TYPE(object);
    switch (type) {
        case HANDLE_TYPE_MUTEX:
        case HANDLE_TYPE_PUSHLOCK:
        case HANDLE_TYPE_EVENT:
        case HANDLE_TYPE_SEMAPHORE:
        case HANDLE_TYPE_PROCESS:
            break;
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    obos_status status = OBOS_STATUS_SUCCESS;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle_desc* object_ptr = OBOS_HandleLookup(OBOS_CurrentHandleTable(), object, 0, true, &status);
    if (obos_is_error(status))
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    struct waitable_header* hdr = object_ptr->un.waitable;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    return Core_WaitOnObject(hdr);
}
// scheduler/process.h

process* Core_LookupProc(uint64_t pid)
{
    process* root = OBOS_KernelProcess;
    while(root)
    {
        process* curr = root;
        if (root->pid == pid)
            return root;
        for (curr = root->children.head; curr;)
        {
            if (curr->pid == pid)
                return curr;

            root = curr->children.head ? curr->children.head : root;
            curr = curr->next;
        }
        if (!curr)
            root = root->parent;
    }
    return nullptr;
}

handle Sys_ProcessOpen(uint64_t pid)
{
    OBOS_UNUSED(pid);
    if (pid > Core_NextPID)
        return HANDLE_INVALID;
    process* proc = Core_LookupProc(pid);
    if (!proc)
        return HANDLE_INVALID;
    proc->refcount++;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle_desc* desc = nullptr;
    handle hnd = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_PROCESS, &desc);
    desc->un.process = proc;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    return hnd;
}

handle Sys_ProcessStart(handle mainThread, handle vmmContext, bool is_fork)
{
    // oh boy...
    obos_status status = OBOS_STATUS_SUCCESS;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle_desc* main_desc =
        HANDLE_TYPE(mainThread) == HANDLE_TYPE_INVALID ?
            nullptr :
            OBOS_HandleLookup(OBOS_CurrentHandleTable(), mainThread, HANDLE_TYPE_THREAD, false, &status);
    if (!main_desc && HANDLE_TYPE(mainThread) != HANDLE_TYPE_INVALID)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return HANDLE_INVALID;
    }
    handle_desc* vmm_ctx_desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), vmmContext, HANDLE_TYPE_VMM_CONTEXT, false, &status);
    if (!vmm_ctx_desc)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return HANDLE_INVALID;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    thread* main = main_desc->un.thread;
    context* vmm_ctx = vmm_ctx_desc->un.vmm_context;

    process* new = Core_ProcessAllocate(nullptr);
    new->ctx = vmm_ctx;
    vmm_ctx->owner = new;
    if (is_fork)
    {
        // Clone current file handles, as well as dirent handles.
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());

        for (size_t i = 0; i < OBOS_CurrentHandleTable()->size; i++)
        {
            handle_desc* hnd = OBOS_CurrentHandleTable()->arr + i;
            switch (hnd->type)
            {
                case HANDLE_TYPE_FD:
                {
                    if (!hnd->un.fd)
                        continue;
                    if (!hnd->un.fd->vn)
                        continue;
                    OBOS_ExpandHandleTable(&new->handles, (i+4) & ~3);
                    handle_desc* new_hnd = &new->handles.arr[i];
                    new_hnd->type = HANDLE_TYPE_FD;
                    new_hnd->un.fd = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(fd), NULL);
                    uint32_t oflags = 0;
                    if (hnd->un.fd->flags & FD_FLAGS_READ)
                        oflags |= FD_OFLAGS_READ;
                    if (hnd->un.fd->flags & FD_FLAGS_WRITE)
                        oflags |= FD_OFLAGS_WRITE;
                    if (hnd->un.fd->flags & FD_FLAGS_UNCACHED)
                        oflags |= FD_OFLAGS_UNCACHED;
                    if (hnd->un.fd->flags & FD_FLAGS_NOEXEC)
                        oflags |= FD_OFLAGS_NOEXEC;
                    Vfs_FdOpenVnode(new_hnd->un.fd, hnd->un.fd->vn, oflags);
                    break;
                }
                case HANDLE_TYPE_DIRENT:
                {
                    OBOS_ExpandHandleTable(&new->handles, (i+4) & ~3);
                    handle_desc* new_hnd = &new->handles.arr[i];
                    new_hnd->type = HANDLE_TYPE_DIRENT;
                    new_hnd->un.dirent = hnd->un.dirent;
                    break;
                }
                default:
                {
                    OBOS_ExpandHandleTable(&new->handles, (i+4) & ~3);
                    handle_desc* new_hnd = &new->handles.arr[i];
                    new_hnd->type = HANDLE_TYPE_INVALID;

                    new_hnd->un.next = new->handles.head;
                    new->handles.head = new_hnd;

                    new_hnd->un.as_int = 0;
                    break;
                }
            }
        }
        new->handles.last_handle = new->handles.size;

        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    }
    Core_ProcessStart(new, main);

    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle_desc* desc = nullptr;
    handle hnd = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_PROCESS, &desc);
    desc->un.process = new;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    if (!is_fork)
        OBOS_OpenStandardFDs(&new->handles);

    return hnd;
}

uint32_t Sys_ProcessGetStatus(handle process)
{
    obos_status status = OBOS_STATUS_SUCCESS;

    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), process, HANDLE_TYPE_PROCESS, false, &status);
    if (!desc)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return UINT32_MAX;
    }
    struct process* proc = desc->un.process;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return proc->exitCode;
}

uint64_t Sys_ProcessGetPID(handle process)
{
    struct process* proc =
        HANDLE_TYPE(process) == HANDLE_TYPE_CURRENT ?
            Core_GetCurrentThread()->proc :
            nullptr;
    if (!proc)
    {
        obos_status status = OBOS_STATUS_SUCCESS;
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), process, HANDLE_TYPE_PROCESS, false, &status);
        if (!desc)
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return UINT32_MAX;
        }
        proc = desc->un.process;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    }
    return proc->pid;
}

uint64_t Sys_ProcessGetPPID(handle process)
{
    struct process* proc =
        HANDLE_TYPE(process) == HANDLE_TYPE_CURRENT ?
            Core_GetCurrentThread()->proc :
            nullptr;
    if (!proc)
    {
        obos_status status = OBOS_STATUS_SUCCESS;
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), process, HANDLE_TYPE_PROCESS, false, &status);
        if (!desc)
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return UINT32_MAX;
        }
        proc = desc->un.process;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    }
    return proc->parent->pid;
}

void OBOS_ThreadHandleFree(handle_desc *hnd)
{
    thread* thr = hnd->un.thread;
    if (thr->snode->free)
        thr->snode->free(thr->snode);
    if (!(--thr->references) && thr->free)
        thr->free(thr);
}

obos_status Sys_WaitProcess(handle proc, int* wstatus, int options, uint32_t* pid)
{
    obos_status status = OBOS_STATUS_SUCCESS;

    process* process = nullptr;

    if (HANDLE_TYPE(proc) == HANDLE_TYPE_ANY)
    {
        process = Core_GetCurrentThread()->proc->children.head;
        while (process && process->dead)
            process = process->next;
        status = process ? OBOS_STATUS_SUCCESS : OBOS_STATUS_NOT_FOUND;
        if (!process)
            return status;
        process->refcount++;
    }
    else
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), proc, HANDLE_TYPE_PROCESS, false, &status);
        if (!desc)
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return status;
        }
        process = desc->un.process;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    }

    status = memcpy_k_to_usr(pid, &process->pid, sizeof(uint32_t));

    if (obos_is_error(status))
        return status;

    if (options & WNOHANG && !process->waiting_threads.signaled)
        return OBOS_STATUS_RETRY;

    again:
    Core_GetCurrentThread()->inWaitProcess = true;
    status = Core_WaitOnObject(WAITABLE_OBJECT(*process));
    Core_GetCurrentThread()->inWaitProcess = false;
    if (obos_is_error(status) && status != OBOS_STATUS_ABORTED)
        return status;

    if ((process->exitCode == 0xffff) && ~options & WCONTINUED)
        goto again;

    if (HANDLE_TYPE(proc) == HANDLE_TYPE_ANY)
        process->refcount--;

    CoreH_ClearSignaledState(WAITABLE_OBJECT(*process));

    if (obos_expect(wstatus != nullptr, true))
        return memcpy_k_to_usr(wstatus, &process->exitCode, sizeof(uint32_t));

    return OBOS_STATUS_SUCCESS;
}
