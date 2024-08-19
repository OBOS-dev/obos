/*
 * oboskrnl/arch/m68k/thread_ctx.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "error.h"
#include <int.h>

#include <scheduler/thread_context_info.h>
#include <stdint.h>

// Thread context manipulation functions.

void CoreS_SetThreadIRQL(thread_ctx* ctx, irql newIRQL)
{
    ctx->irql = newIRQL;
}
irql CoreS_GetThreadIRQL(const thread_ctx* ctx)
{
    return ctx->irql;
}
void* CoreS_GetThreadStack(const thread_ctx* ctx)
{
    return ctx->stackBase;
}
size_t CoreS_GetThreadStackSize(const thread_ctx* ctx)
{
    return ctx->stackSize;
}
obos_status CoreS_SetupThreadContext(thread_ctx* ctx, uintptr_t entry, uintptr_t arg1, bool makeUserMode, void* stackBase, size_t stackSize)
{
    if (!ctx)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ctx->pc = entry;
    if (!makeUserMode)
        ctx->sr |= (1<<13);
    ctx->sp = (uintptr_t)stackBase+stackSize;
    ctx->sp -= 4;
    *(uintptr_t*)ctx->sp = arg1;
    ctx->sp -= 4;
    *(uintptr_t*)ctx->sp = 0;
    ctx->stackBase = stackBase;
    ctx->stackSize = stackSize;
    return OBOS_STATUS_SUCCESS;
}
obos_status CoreS_FreeThreadContext(thread_ctx* ctx)
{
    OBOS_UNUSED(ctx);
    return OBOS_STATUS_SUCCESS;
}