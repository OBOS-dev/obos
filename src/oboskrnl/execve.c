/*
 * oboskrnl/execve.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <execve.h>
#include <signal.h>
#include <handle.h>
#include <syscall.h>

#include <elf/load.h>
#include <elf/elf.h>

#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>

#include <locks/mutex.h>
#include <locks/wait.h>

#include <allocators/base.h>

#include <mm/alloc.h>

#include <asan.h>

// vec is terminated with a nullptr entry.
static char* const* allocate_user_vector_as_kernel(context* ctx, char* const* vec, size_t* const szvec, obos_status* status)
{
    char* const* kvec = Mm_MapViewOfUserMemory(ctx, (void*)vec, nullptr, OBOS_PAGE_SIZE, OBOS_PROTECTION_READ_ONLY, true, status);
    if (!kvec)
        return nullptr;
    char* const* iter = kvec;
    uintptr_t offset = (uintptr_t)kvec-(uintptr_t)iter;
    size_t currSize = OBOS_PAGE_SIZE;
    while ((*iter++) != 0)
    {
        if (OBOS_CROSSES_PAGE_BOUNDARY(iter, sizeof(*iter)*2))
        {
            Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)kvec, currSize);
            currSize += OBOS_PAGE_SIZE;
            kvec = Mm_MapViewOfUserMemory(ctx, (void*)(iter+1), nullptr, currSize, OBOS_PROTECTION_READ_ONLY, true, status);
            if (!kvec)
                return nullptr;
            iter = (char**)((uintptr_t)kvec + offset);
        }
        offset += 8;
        (*szvec)++;
    }
    return kvec;
}

static char* const* reallocate_user_vector_as_kernel(context* ctx, char* const* vec, size_t count, obos_status* statusp)
{
    OBOS_UNUSED(ctx);

    char** ret = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, count+1, sizeof(char*), statusp);
    if (!ret)
        return nullptr;

    for (size_t i = 0; i < count; i++)
    {
        size_t str_len = 0;
        obos_status status = OBOSH_ReadUserString(vec[i], nullptr, &str_len);
        if (obos_is_error(status))
        {
            OBOS_KernelAllocator->Free(OBOS_KernelAllocator, ret, count*sizeof(char*));
            if (statusp) *statusp = status;
            return nullptr;
        }
        char* buf = OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, str_len+1, nullptr);
        OBOSH_ReadUserString(vec[i], buf, &str_len);
        ret[i] = buf;
    }
    Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)vec, count*sizeof(char*));
    return ret;
}

obos_status Sys_ExecVE(const void* buf, size_t szBuf, char* const* argv, char* const* envp)
{
    if (!OBOSS_HandControlTo)
        return OBOS_STATUS_UNIMPLEMENTED;
    if (!buf || !szBuf)
        return OBOS_STATUS_INVALID_ARGUMENT;

    obos_status status = OBOS_STATUS_SUCCESS;

    context* ctx = CoreS_GetCPULocalPtr()->currentContext;

    size_t argc = 0;
    size_t envpc = 0;

    char* const* kargv = allocate_user_vector_as_kernel(ctx, argv, &argc, &status);
    if (obos_is_error(status))
        return status;

    char* const* knvp = allocate_user_vector_as_kernel(ctx, envp, &envpc, &status);
    if (obos_is_error(status))
        return status;

    // Reallocate kargv+knvp to only use kernel pointers
    kargv = reallocate_user_vector_as_kernel(ctx, kargv, argc, &status);
    if (obos_is_error(status))
        return status;

    knvp = reallocate_user_vector_as_kernel(ctx, knvp, envpc, &status);
    if (obos_is_error(status))
        return status;

    const void* kbuf = Mm_MapViewOfUserMemory(ctx, (void*)buf, nullptr, szBuf, OBOS_PROTECTION_READ_ONLY, false, &status);
    status = OBOS_LoadELF(ctx, kbuf, szBuf, nullptr, true, false);
    if (obos_is_error(status))
        return status;

    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    // For each thread in the current process, send SIGKILL, and wait for it to die.
    for (thread_node* curr = Core_GetCurrentThread()->proc->threads.head; curr; )
    {
        thread* const thr = curr->data;
        curr = curr->next;
        if (thr == Core_GetCurrentThread())
            continue;

        OBOS_Kill(Core_GetCurrentThread(), thr, SIGKILL);
        while (~thr->flags & THREAD_FLAGS_DIED)
            OBOSS_SpinlockHint();
    }
    Core_LowerIrql(oldIrql);

    // Reset all signals to SIG_DFL
    sigaction sigact = {.un.handler = SIG_DFL};
    for (int sigval = 1; sigval <= SIGMAX; sigval++)
        OBOS_SigAction(sigval, &sigact, nullptr);

    // TODO: Cancel any outstanding async I/O.
    // NOTE(oberrow): POSIX doesn't mandate this, but it is better do so.

    // Close the handles of the following types:
    // - dirent
    // - timer
    // - driver_id
    // - thread_ctx
    // - context

    handle_table* tbl = &CoreS_GetCPULocalPtr()->currentThread->proc->handles;

    for (size_t i = 0; i < tbl->size; i++)
    {
        switch (tbl->arr[i].type) {
            case HANDLE_TYPE_DIRENT:
            case HANDLE_TYPE_TIMER:
            case HANDLE_TYPE_THREAD_CTX:
            case HANDLE_TYPE_VMM_CONTEXT:
                Sys_HandleClose(i | (tbl->arr[i].type << HANDLE_TYPE_SHIFT));
                break;
            case HANDLE_TYPE_FD:
                if (tbl->arr[i].un.fd->flags & FD_FLAGS_NOEXEC)
                    Sys_HandleClose(i | (tbl->arr[i].type << HANDLE_TYPE_SHIFT));
                break;
            default:
                break;
        }
    }

    // Free all memory
    page_range* rng = nullptr;
    page_range* next = nullptr;
    RB_FOREACH_SAFE(rng, page_tree, &ctx->pages, next)
        Mm_VirtualMemoryFree(ctx, (void*)rng->virt, rng->size);

    // Load the ELF into the process.
    elf_info info = {};
    OBOS_LoadELF(ctx, kbuf, szBuf, &info, false, false);

    struct exec_aux_values aux = {.elf=info};
    const Elf_Ehdr* ehdr = kbuf;
    aux.phdr.ptr = (void*)((uintptr_t)info.base + ehdr->e_phoff);
    aux.phdr.phent = ehdr->e_phentsize;
    aux.phdr.phnum = ehdr->e_phnum;

    Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)kbuf, szBuf);

    aux.argc = argc;
    aux.argv = argv;

    aux.envp = envp;
    aux.envpc = envpc;

    OBOSS_HandControlTo(ctx, &aux);
}
