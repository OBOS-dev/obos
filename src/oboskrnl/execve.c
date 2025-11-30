/*
 * oboskrnl/execve.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <error.h>
#include <execve.h>
#include <signal.h>
#include <handle.h>
#include <syscall.h>

#include <elf/load.h>
#include <elf/elf.h>

#include <irq/irql.h>

#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>
#include <scheduler/thread_context_info.h>

#include <locks/mutex.h>
#include <locks/wait.h>

#include <allocators/base.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>

#include <asan.h>

#undef OBOS_CROSSES_PAGE_BOUNDARY

static bool OBOS_CROSSES_PAGE_BOUNDARY(void* ptr_, size_t sz)
{
    uintptr_t ptr = (uintptr_t)ptr_;
    uintptr_t limit = ptr+(sz-1);
    limit -= (limit % OBOS_PAGE_SIZE);
    ptr -= (ptr % OBOS_PAGE_SIZE);
    return ptr != limit;
}

// vec is terminated with a nullptr entry.
static char** allocate_user_vector_as_kernel(context* ctx, char* const* vec, size_t* const szvec, obos_status* status)
{
    char** kstr = Mm_MapViewOfUserMemory(ctx, (void*)((uintptr_t)vec - ((uintptr_t)vec % OBOS_PAGE_SIZE)), nullptr, OBOS_PAGE_SIZE, OBOS_PROTECTION_READ_ONLY, true, status);
    if (!kstr)
        return nullptr;
    kstr = (void*)(((uintptr_t)kstr) + (uintptr_t)vec % OBOS_PAGE_SIZE);

    char** iter = kstr;
    char* const* uiter = vec;
    OBOS_UNUSED(uiter);
    uintptr_t offset = (uintptr_t)kstr-(uintptr_t)iter;
    size_t currSize = OBOS_PAGE_SIZE;

    while ((*iter++) != 0)
    {
        if (OBOS_CROSSES_PAGE_BOUNDARY(iter, sizeof(*iter)))
        {
            Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)kstr, currSize);
            currSize += OBOS_PAGE_SIZE;
            kstr = Mm_MapViewOfUserMemory(ctx, (void*)((uintptr_t)vec + (offset % OBOS_PAGE_SIZE) + sizeof(*iter)), nullptr, currSize, OBOS_PROTECTION_READ_ONLY, true, status);
            if (!kstr)
                return nullptr;
            iter = (char**)((uintptr_t)kstr + (offset % OBOS_PAGE_SIZE));
        }
        offset += sizeof(*iter);
        (*szvec)++;
        uiter++;
    }

    return kstr;
}

static char** reallocate_user_vector_as_kernel(context* ctx, char* const* vec, size_t count, obos_status* statusp)
{
    OBOS_UNUSED(ctx);

    char** ret = ZeroAllocate(OBOS_KernelAllocator, count+1, sizeof(char*), statusp);
    if (!ret)
        return nullptr;

    for (size_t i = 0; i < count; i++)
    {
        size_t str_len = 0;
        if (!vec[i])
        {
            ret[i] = 0;
            break;
        }
        obos_status status = OBOSH_ReadUserString(vec[i], nullptr, &str_len);
        if (obos_is_error(status))
        {
            Free(OBOS_KernelAllocator, ret, count*sizeof(char*));
            if (statusp) *statusp = status;
            return nullptr;
        }
        char* buf = Allocate(OBOS_KernelAllocator, str_len+1, nullptr);
        OBOSH_ReadUserString(vec[i], buf, &str_len);
        buf[str_len] = 0;
        ret[i] = buf;
    }
    Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)vec, count*sizeof(char*));
    return ret;
}

obos_status Sys_ExecVE(const char* upath, char* const* argv, char* const* envp)
{
    if (!OBOSS_HandControlTo)
        return OBOS_STATUS_UNIMPLEMENTED;
    if (!upath)
        return OBOS_STATUS_INVALID_ARGUMENT;

    obos_status status = OBOS_STATUS_SUCCESS;

    context* ctx = CoreS_GetCPULocalPtr()->currentContext;

    size_t argc = 0;
    size_t envpc = 0;

    // Read the file.
    char* path = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(upath, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    path = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(upath, path, nullptr);
    fd file = {};
    status = Vfs_FdOpen(&file, path, FD_OFLAGS_READ|FD_OFLAGS_EXECUTE);
    if (Core_GetCurrentThread()->proc->exec_file)
        Free(OBOS_KernelAllocator, Core_GetCurrentThread()->proc->exec_file, strlen(Core_GetCurrentThread()->proc->exec_file)+1);
    if (obos_is_error(status))
        return status;
    Core_GetCurrentThread()->proc->exec_file = VfsH_DirentPath(VfsH_DirentLookup(path), nullptr);
    size_t szBuf = file.vn->filesize;
    void* kbuf = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, szBuf, OBOS_PROTECTION_READ_ONLY, 0, &file, nullptr);
    bool set_uid = file.vn->perm.set_uid;
    bool set_gid = file.vn->perm.set_gid;
    uid target_euid = set_uid ? file.vn->uid : Core_GetCurrentThread()->proc->euid;
    gid target_egid = set_gid ? file.vn->gid : Core_GetCurrentThread()->proc->egid;
    Vfs_FdClose(&file);

    char** kargv = allocate_user_vector_as_kernel(ctx, argv, &argc, &status);
    if (obos_is_error(status))
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, szBuf);
        return status;
    }

    char** knvp = allocate_user_vector_as_kernel(ctx, envp, &envpc, &status);
    if (obos_is_error(status))
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, szBuf);
        return status;
    }

    // Reallocate kargv+knvp to only use kernel pointers
    kargv = reallocate_user_vector_as_kernel(ctx, kargv, argc, &status);
    if (obos_is_error(status))
        return status;

    knvp = reallocate_user_vector_as_kernel(ctx, knvp, envpc, &status);
    if (obos_is_error(status))
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, szBuf);
        return status;
    }

    do {
        if (Core_GetCurrentThread()->proc->cmdline)
            Free(OBOS_KernelAllocator, Core_GetCurrentThread()->proc->cmdline, strlen(Core_GetCurrentThread()->proc->cmdline)+1);
        string cmd_line = {};
        process* proc = Core_GetCurrentThread()->proc;
        OBOS_InitString(&cmd_line, proc->exec_file);
        for (size_t i = 1; i < argc; i++)
        {
            size_t len_arg = strlen(kargv[i]);
            if (strchr(kargv[i], ' ') == len_arg)
            {
                OBOS_AppendStringC(&cmd_line, " ");
                OBOS_AppendStringC(&cmd_line, kargv[i]);
            }
            else
            {
                OBOS_AppendStringC(&cmd_line, " \"");
                OBOS_AppendStringC(&cmd_line, kargv[i]);
                OBOS_AppendStringC(&cmd_line, "\"");
            }
        }
        proc->cmdline = memcpy(Allocate(OBOS_KernelAllocator, OBOS_GetStringSize(&cmd_line)+1, nullptr), 
                               OBOS_GetStringCPtr(&cmd_line), 
                              OBOS_GetStringSize(&cmd_line)+1);
        OBOS_FreeString(&cmd_line);
    } while(0);

    status = OBOS_LoadELF(ctx, kbuf, szBuf, nullptr, true, false);
    if (obos_is_error(status))
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, szBuf);
        return status;
    }

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
    Core_GetCurrentThread()->signal_info->pending = 0;
    Core_GetCurrentThread()->signal_info->mask = 0;

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
                if (!tbl->arr[i].un.fd)
                    break;
                if (tbl->arr[i].un.fd->flags & FD_FLAGS_NOEXEC)
                    Sys_HandleClose(i | (tbl->arr[i].type << HANDLE_TYPE_SHIFT));
                break;
            default:
                break;
        }
    }

    oldIrql = Core_RaiseIrql(IRQL_DISPATCH);

    // Free all memory
    page_range* rng = nullptr;
    page_range* next = nullptr;
    
    for ((rng) = RB_MIN(page_tree, &ctx->pages); (rng) != nullptr; )
    {
        next = RB_NEXT(page_tree, &ctx->pages, rng);
        uintptr_t virt = rng->virt;
        if (rng->hasGuardPage)
            virt += (rng->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        uintptr_t limit = rng->virt+rng->size;
        Mm_VirtualMemoryFree(ctx, (void*)virt, limit-virt);
        rng = next;
    }

    // Load the ELF into the process.
    elf_info info = {};
    status = OBOS_LoadELF(ctx, kbuf, szBuf, &info, false, false);
    if (obos_is_error(status))
    {
        OBOS_Error("OBOS_LoadELF failed in %s after already having verified ELF file status. Status: %d\n", __func__, status);
        Core_ExitCurrentProcess(9 | (9<<8));
//      OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "OBOS_LoadELF failed in %s after already having verified ELF file status.\nStatus: %d\n", __func__, status);
    }

    struct exec_aux_values aux = {.elf=info};
    const Elf_Ehdr* ehdr = kbuf;
    aux.phdr.ptr = (void*)((uintptr_t)info.base + ehdr->e_phoff);
    aux.phdr.phent = ehdr->e_phentsize;
    aux.phdr.phnum = ehdr->e_phnum;

    Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)kbuf, szBuf);

    Core_GetCurrentThread()->context.stackBase = Mm_VirtualMemoryAlloc(ctx, nullptr, 4*1024*1024, OBOS_PROTECTION_USER_PAGE, VMA_FLAGS_GUARD_PAGE, nullptr, nullptr);
    Core_GetCurrentThread()->context.stackSize = 4*1024*1024;
    Core_GetCurrentThread()->userStack = Mm_VirtualMemoryAlloc(Core_GetCurrentThread()->proc->ctx, nullptr, 0x10000, 0, VMA_FLAGS_GUARD_PAGE, nullptr, nullptr);

    aux.argc = argc;
    aux.argv = kargv;

    aux.envp = knvp;
    aux.envpc = envpc;

    aux.at_secure = set_gid || set_uid;

    Core_GetCurrentThread()->proc->euid = target_euid;
    Core_GetCurrentThread()->proc->egid = target_egid;
    Core_GetCurrentThread()->proc->sgid = target_euid;
    Core_GetCurrentThread()->proc->sgid = target_egid;

    OBOSS_HandControlTo(ctx, &aux);
}
