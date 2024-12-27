/*
 * oboskrnl/arch/x86_64/execve.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <memmanip.h>
#include <klog.h>
#include <execve.h>

#include <scheduler/thread_context_info.h>
#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <mm/context.h>
#include <mm/alloc.h>

#include <irq/irql.h>

typedef struct
{
    int a_type;
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

static void write_vector_to_stack(char* const* vec, char** stck_buf, size_t cnt)
{
    for (size_t i = 0; i < cnt; i++)
    {
        size_t str_len = strlen(vec[i]);
        char* str = CoreS_ThreadAlloca(&Core_GetCurrentThread()->context, str_len+1, nullptr);
        if (!str)
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "obos shat itself: your stack is not big enough to hold all these arguments");
        memcpy_k_to_usr(str, vec[i], str_len);
        stck_buf[i] = str;
    }
}

OBOS_NORETURN void OBOSS_HandControlTo(struct context* ctx, struct exec_aux_values* aux)
{
    (void)Core_RaiseIrql(IRQL_DISPATCH);
    Core_GetCurrentThread()->context.frame.rsp = (uintptr_t)Core_GetCurrentThread()->context.stackBase + Core_GetCurrentThread()->context.stackSize;
    // 1+argc+char*[argc]+NULL+char*[envpc]+NULL+4(aux)+1
    // 1+1+argc+1+envpc+1+8+1=9+envpc+argc
    size_t szAllocation = (13+aux->envpc+aux->argc)*sizeof(uint64_t);
    void* uinit_vals = CoreS_ThreadAlloca(&Core_GetCurrentThread()->context, szAllocation, nullptr);
    uint64_t* init_vals = Mm_MapViewOfUserMemory(ctx, uinit_vals, nullptr, szAllocation, 0, false, nullptr);
    init_vals[0] = aux->argc;
    init_vals[1+aux->argc+1] = 0;

    auxv_t* auxv = (auxv_t*)&init_vals[1+aux->argc+2+aux->envpc+2];

    auxv[0].a_type = AT_PHDR;
    auxv[0].a_un.a_ptr = aux->phdr.ptr;

    auxv[1].a_type = AT_PHENT;
    auxv[1].a_un.a_val = aux->phdr.phent;

    auxv[2].a_type = AT_PHNUM;
    auxv[2].a_un.a_val = aux->phdr.phnum;

    auxv[3].a_type = AT_ENTRY;
    auxv[3].a_un.a_ptr = (void*)aux->elf.entry;

    auxv[4].a_type = AT_NULL;

    write_vector_to_stack(aux->argv, (char**)(init_vals+1), aux->argc);
    write_vector_to_stack(aux->envp, (char**)(init_vals+1+aux->argc+2), aux->envpc);
    init_vals[1+aux->argc+2+aux->envpc+1] = 0;

    Core_GetCurrentThread()->context.frame.cr3 = ctx->pt;
    Core_GetCurrentThread()->context.frame.cs = 0x18|3;
    Core_GetCurrentThread()->context.frame.ss = 0x20|3;
    Core_GetCurrentThread()->context.frame.rip = aux->elf.real_entry;
    Core_GetCurrentThread()->context.frame.rflags = 0x200202; // CPUID,IF
    Core_GetCurrentThread()->context.irql = 0;
    Core_GetCurrentThread()->context.fs_base = 0;
    Core_GetCurrentThread()->context.gs_base = 0;

    // TODO: Reset extended context info.

    CoreS_SwitchToThreadContext(&Core_GetCurrentThread()->context);
}
