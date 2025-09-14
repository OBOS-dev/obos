/*
 * oboskrnl/arch/m68k/execve.c
 *
 * Copyright (c) 2025 Omar Berrow
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

#include <irq/irql.h>

#include <vfs/fd_sys.h>

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
};

static void allocate_string_vector_on_stack(char** vec, size_t cnt)
{
    OBOS_ENSURE(CoreS_ThreadAlloca != nullptr);
    for (size_t i = 0; i < cnt; i++)
    {
        size_t str_len = strlen(vec[i]);
        char* str = CoreS_ThreadAlloca(&Core_GetCurrentThread()->context, str_len+1, nullptr);
        if (!str)
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "obos shat itself: your stack is not big enough to hold all these arguments");
        memcpy_k_to_usr(str, vec[i], str_len+1);
        // stck_buf[i] = str;
        vec[i] = str;
    }
}

static void write_vector_to_stack(char** vec, char** stck_buf, size_t cnt)
{
    for (size_t i = 0; i < cnt; i++)
        stck_buf[i] = vec[i];
    stck_buf[cnt] = 0;
}

static void reset_extended_state()
{
    // TODO: Implement
    OBOS_Debug("m68k execve: %s unimplemented", __func__);
}

OBOS_NORETURN void OBOSS_HandControlTo(struct context* ctx, struct exec_aux_values* aux)
{
    if (Core_GetIrql() < IRQL_DISPATCH)
    {
        irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
        OBOS_UNUSED(oldIrql);
    }

    Core_GetCurrentThread()->context.usp = (uintptr_t)Core_GetCurrentThread()->context.stackBase + Core_GetCurrentThread()->context.stackSize;
    // 1+argc+char*[argc]+NULL+char*[envpc]+NULL+4(aux)+1
    // 1+1+argc+1+envpc+1+8+1=9+envpc+argc
    size_t szAllocation = (13+aux->envpc+aux->argc)*sizeof(uint64_t);
    allocate_string_vector_on_stack(aux->argv, aux->argc);
    allocate_string_vector_on_stack(aux->envp, aux->envpc);
    Core_GetCurrentThread()->context.usp &= ~0xf;
    if (!((aux->argc+aux->envpc) % 2))
        Core_GetCurrentThread()->context.usp -= 8;

    void* uinit_vals = CoreS_ThreadAlloca(&Core_GetCurrentThread()->context, szAllocation, nullptr);
    uint32_t* init_vals = Mm_MapViewOfUserMemory(ctx, uinit_vals, nullptr, szAllocation, 0, false, nullptr);
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

    OBOS_Debug("Handing off control to user program.\n");
    OBOS_Debug("NOTE: SP=0x%p.\n", Core_GetCurrentThread()->context.usp);

    uintptr_t udata[2] = { aux->elf.real_entry, ctx->pt };
    CoreS_SetKernelStack(Core_GetCurrentThread()->kernelStack);
    Core_GetCurrentThread()->context.pc = udata[0];
    Core_GetCurrentThread()->context.urp = udata[1];
    Core_GetCurrentThread()->context.a6 = Core_GetCurrentThread()->context.usp;
    Core_GetCurrentThread()->context.sr &= ~BIT(13);
    CoreS_SwitchToThreadContext(&Core_GetCurrentThread()->context);
    OBOS_UNREACHABLE;
}

void OBOSS_HandOffToInit(struct exec_aux_values* aux)
{
    OBOS_OpenStandardFDs(OBOS_CurrentHandleTable());

    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    OBOS_UNUSED(oldIrql);
    context* ctx = CoreS_GetCPULocalPtr()->currentContext;

    Core_GetCurrentThread()->context.stackBase = Mm_VirtualMemoryAlloc(ctx, nullptr, 4*1024*1024, OBOS_PROTECTION_USER_PAGE, VMA_FLAGS_GUARD_PAGE, nullptr, nullptr);
    Core_GetCurrentThread()->context.stackSize = 4*1024*1024;

    CoreS_SetThreadPageTable(&Core_GetCurrentThread()->context, ctx->pt);

    OBOSS_HandControlTo(ctx, aux);
}
