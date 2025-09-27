/*
 * oboskrnl/init_proc.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <cmdline.h>
#include <memmanip.h>
#include <execve.h>
#include <error.h>
#include <init_proc.h>
#include <signal.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <scheduler/process.h>
#include <scheduler/thread.h>
#include <scheduler/schedule.h>
#include <scheduler/thread_context_info.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>

#include <allocators/base.h>

#include <elf/load.h>
#include <elf/elf.h>

static struct exec_aux_values aux;

void OBOS_LoadInit()
{
    if (!Core_GetCurrentThread()->proc->controlling_tty)
    {
        OBOS_Error("%s: Cannot load init due to non-existent controlling tty.\n", __func__);
        return;
    }

    bool should_load_init = OBOS_GetOPTF("no-init");
    if (should_load_init)
    {
        OBOS_Log("%s: Not loading init due to kernel command line option 'no-init'\n", __func__);
        return;
    }

    // Freed while handing off control to init.
    char* init_path = OBOS_GetOPTS("init-path");
    fd init_fd = {};

    if (!init_path)
        init_path = memcpy(Allocate(OBOS_KernelAllocator, 6, nullptr), "/init", 6);

    OBOS_Log("Loading %s\n", init_path);

    obos_status status = Vfs_FdOpen(&init_fd, init_path, FD_OFLAGS_READ|FD_OFLAGS_EXECUTE);
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not open %s. Status: %d\n", init_path, status);

    context* new_ctx = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(context), nullptr);
    Mm_ConstructContext(new_ctx);
    process* new = Core_ProcessAllocate(nullptr);
    new->ctx = new_ctx;
    new_ctx->owner = new;
    new_ctx->workingSet.capacity = 64*1024*1024;
    Core_ProcessStart(new, nullptr);
    new->exec_file = VfsH_DirentPath(VfsH_DirentLookup(init_path), nullptr);

    void* buf = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, init_fd.vn->filesize, OBOS_PROTECTION_READ_ONLY, VMA_FLAGS_PRIVATE, &init_fd, nullptr);
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not map init program. Status: %d\n", status);

    status = OBOS_LoadELF(new_ctx, buf, init_fd.vn->filesize, &aux.elf, false, false);
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not load %s. Status: %d\n", init_path, status);

    OBOS_Log("Loaded %s at 0x%p\n", init_path, aux.elf.base);
    OBOS_Debug("NOTE: Dynamic linker loaded at 0x%p\n", aux.elf.rtld_base);

    const Elf_Ehdr* ehdr = buf;
    aux.phdr.ptr = (void*)((uintptr_t)aux.elf.base + ehdr->e_phoff);
    aux.phdr.phent = ehdr->e_phentsize;
    aux.phdr.phnum = ehdr->e_phnum;

    aux.argc = 1+OBOS_InitArgumentsCount;
    aux.argv = ZeroAllocate(OBOS_KernelAllocator, aux.argc+1, sizeof(char*), nullptr);
    aux.argv[0] = init_path;
    for (size_t i = 1; i < (OBOS_InitArgumentsCount+1); i++)
        aux.argv[i] = OBOS_argv[OBOS_InitArgumentsStart+i-1];

    do {
        if (Core_GetCurrentThread()->proc->cmdline)
            Free(OBOS_KernelAllocator, Core_GetCurrentThread()->proc->cmdline, strlen(Core_GetCurrentThread()->proc->cmdline)+1);
        string cmd_line = {};
        process* proc = new;
        OBOS_InitString(&cmd_line, init_path);
        for (size_t i = 1; i < aux.argc; i++)
        {
            size_t len_arg = strlen(aux.argv[i]);
            if (strchr(aux.argv[i], ' ') == len_arg)
            {
                OBOS_AppendStringC(&cmd_line, " ");
                OBOS_AppendStringC(&cmd_line, aux.argv[i]);
            }
            else
            {
                OBOS_AppendStringC(&cmd_line, " \"");
                OBOS_AppendStringC(&cmd_line, aux.argv[i]);
                OBOS_AppendStringC(&cmd_line, "\"");
            }
        }
        proc->cmdline = memcpy(Allocate(OBOS_KernelAllocator, OBOS_GetStringSize(&cmd_line)+1, nullptr), 
                                OBOS_GetStringCPtr(&cmd_line), 
                                OBOS_GetStringSize(&cmd_line)+1);
        OBOS_FreeString(&cmd_line);
    } while(0);

    aux.envp = nullptr;
    aux.envpc = 0;

    thread* thr = CoreH_ThreadAllocate(nullptr);
    thread_ctx thr_ctx = {};

    thr->kernelStack = Mm_AllocateKernelStack(new_ctx, &status);
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not allocate kernel stack for init program.\n");

    CoreS_SetupThreadContext(&thr_ctx, (uintptr_t)OBOSS_HandOffToInit, (uintptr_t)&aux, false, thr->kernelStack, 0x10000);

    if (new->controlling_tty)
        new->controlling_tty->fg_job = new;

    // CoreS_SetThreadPageTable(&thr_ctx, new_ctx->pt);

    CoreH_ThreadInitialize(thr, THREAD_PRIORITY_NORMAL, Core_DefaultThreadAffinity, &thr_ctx);

    Core_ProcessAppendThread(new, thr);

    thr->signal_info = OBOSH_AllocateSignalHeader();

    CoreH_ThreadReady(thr);
}
