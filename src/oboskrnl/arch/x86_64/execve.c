/*
 * oboskrnl/arch/x86_64/execve.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <memmanip.h>
#include <klog.h>
#include <execve.h>
#include <init_proc.h>
#include <handle.h>

#include <scheduler/thread_context_info.h>
#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <mm/context.h>
#include <mm/alloc.h>

#include <arch/x86_64/sse.h>

#include <irq/irql.h>

#include <vfs/fd_sys.h>

typedef struct
{
    OBOS_ALIGNAS(0x8) uint64_t a_type;
    union {
        long a_val;
        void *a_ptr;
        void (*a_fnc)();
    } a_un;
} auxv_t;

enum {
    AT_NULL,
    AT_IGNORE,
    AT_EXECFD,
    AT_PHDR,
    AT_PHENT,
    AT_PHNUM,
    AT_PAGESZ,
    AT_BASE,
    AT_FLAGS,
    AT_ENTRY,
    AT_NOTELF,
    AT_UID,
    AT_EUID,
    AT_GID,
    AT_EGID,
    AT_PLATFORM,
    AT_HWCAP,
    AT_CLKTCK,
    AT_SECURE = 23,
    AT_BASE_PLATFORM,
    AT_RANDOM,
    AT_HWCAP2,
    AT_EXECFN = 31,
};

static void allocate_string_vector_on_stack(char** vec, size_t cnt)
{
    for (size_t i = 0; i < cnt; i++)
    {
        size_t str_len = strlen(vec[i]);
        char* str = CoreS_ThreadAlloca(&Core_GetCurrentThread()->context, str_len+1, nullptr);
        if (!str)
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "obos shat itself: your stack is not big enough to hold all these arguments");
        memcpy_k_to_usr(str, vec[i], str_len);
        // stck_buf[i] = str;
        vec[i] = str;
    }
}

static void write_vector_to_stack(char** vec, char** stck_buf, size_t cnt)
{
    for (size_t i = 0; i < cnt; i++)
    {
        printf("%p\n", vec[i]);
        stck_buf[i] = vec[i];
    }
    stck_buf[cnt] = 0;
}

OBOS_NORETURN void Arch_GotoUser(uintptr_t rip, uintptr_t cr3, uintptr_t rsp);
uintptr_t Arch_GotoUserBootstrap(uintptr_t udata)
{
    uintptr_t* user = (void*)udata;
    Arch_GotoUser(user[0], user[1], user[2]);
    return -1;
}

OBOS_NORETURN void OBOSS_HandControlTo(struct context* ctx, struct exec_aux_values* aux)
{
    if (Core_GetIrql() < IRQL_DISPATCH)
        (void)Core_RaiseIrql(IRQL_DISPATCH);
    Core_GetCurrentThread()->context.extended_ctx_ptr = Arch_AllocateXSAVERegion();

    Core_GetCurrentThread()->context.frame.rsp = (uintptr_t)Core_GetCurrentThread()->context.stackBase + Core_GetCurrentThread()->context.stackSize;
    // 1+argc+char*[argc]+NULL+char*[envpc]+NULL+4(aux)+1
    // 1+1+argc+1+envpc+1+8+1=9+envpc+argc
    size_t szAllocation = (13+aux->envpc+aux->argc)*sizeof(uint64_t);
    allocate_string_vector_on_stack(aux->argv, aux->argc);
    allocate_string_vector_on_stack(aux->envp, aux->envpc);
    Core_GetCurrentThread()->context.frame.rsp &= ~0xf;
    if (!((aux->argc+aux->envpc) % 2))
        Core_GetCurrentThread()->context.frame.rsp -= 8;

    void* uinit_vals = CoreS_ThreadAlloca(&Core_GetCurrentThread()->context, szAllocation, nullptr);
    uint64_t* init_vals = Mm_MapViewOfUserMemory(ctx, uinit_vals, nullptr, szAllocation, 0, false, nullptr);
    init_vals[0] = aux->argc;

    auxv_t* auxv = (auxv_t*)&init_vals[1+aux->argc+1+aux->envpc+1];

    (*auxv).a_type = AT_PHDR;
    (*auxv).a_un.a_ptr = aux->phdr.ptr;
    auxv++;

    (*auxv).a_type = AT_PHENT;
    (*auxv).a_un.a_val = aux->phdr.phent;
    auxv++;

    (*auxv).a_type = AT_PHNUM;
    (*auxv).a_un.a_val = aux->phdr.phnum;
    auxv++;

    (*auxv).a_type = AT_ENTRY;
    (*auxv).a_un.a_ptr = (void*)aux->elf.entry;
    auxv++;

    (*auxv).a_type = AT_NULL;

    write_vector_to_stack(aux->argv, (char**)(init_vals+1), aux->argc);
    write_vector_to_stack(aux->envp, (char**)(init_vals+1+aux->argc+1), aux->envpc);

    // Core_GetCurrentThread()->context.frame.cr3 = ctx->pt;
    // Core_GetCurrentThread()->context.frame.cs = 0x20|3;
    // Core_GetCurrentThread()->context.frame.ss = 0x18|3;
    // Core_GetCurrentThread()->context.frame.rip = aux->elf.real_entry;
    // Core_GetCurrentThread()->context.frame.rflags = 0x200202; // CPUID,IF
    // Core_GetCurrentThread()->context.irql = 0;
    // Core_GetCurrentThread()->context.fs_base = 0;
    // Core_GetCurrentThread()->context.gs_base = 0;

    // TODO: Reset extended context info.

    Core_GetCurrentThread()->context.frame.rbp = 0;
    uintptr_t udata[3] = { aux->elf.real_entry, ctx->pt, Core_GetCurrentThread()->context.frame.rsp };
    CoreS_SetKernelStack(Core_GetCurrentThread()->kernelStack);
    CoreS_CallFunctionOnStack(Arch_GotoUserBootstrap, (uintptr_t)&udata);
    OBOS_UNREACHABLE;
}

void OBOSS_HandOffToInit(struct exec_aux_values* aux)
{
    OBOS_OpenStandardFDs(OBOS_CurrentHandleTable());

    (void)Core_RaiseIrql(IRQL_DISPATCH);
    context* ctx = CoreS_GetCPULocalPtr()->currentContext;

    Core_GetCurrentThread()->context.stackBase = Mm_VirtualMemoryAlloc(ctx, nullptr, 4*1024*1024, 0, VMA_FLAGS_GUARD_PAGE, nullptr, nullptr);
    Core_GetCurrentThread()->context.stackSize = 4*1024*1024;

    CoreS_SetThreadPageTable(&Core_GetCurrentThread()->context, ctx->pt);

    OBOSS_HandControlTo(ctx, aux);
}
