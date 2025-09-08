/*
 * oboskrnl/arch/m68k/thread_ctx.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>

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
    {
        ctx->sr |= (1<<13);
        ctx->sp = (uintptr_t)stackBase+stackSize;
        ctx->sp -= 4;
        *(uintptr_t*)ctx->sp = arg1;
        ctx->sp -= 4;
        *(uintptr_t*)ctx->sp = 0;
    }
    else 
    {
        ctx->usp = (uintptr_t)stackBase+stackSize;
        uint32_t stack_frame[2] = {
            0,
            arg1,
        };
        ctx->usp -= 8;
        memcpy_k_to_usr((void*)ctx->usp, stack_frame, sizeof(stack_frame));
        // ctx->usp -= 4;
        // *(uintptr_t*)ctx->usp = arg1;
        // ctx->usp -= 4;
        // *(uintptr_t*)ctx->usp = 0;
    }
    ctx->stackBase = stackBase;
    ctx->stackSize = stackSize;
    return OBOS_STATUS_SUCCESS;
}
obos_status CoreS_FreeThreadContext(thread_ctx* ctx)
{
    OBOS_UNUSED(ctx);
    return OBOS_STATUS_SUCCESS;
}
void CoreS_SetThreadPageTable(thread_ctx* ctx, page_table pt)
{
    if (!pt || !ctx)
        return;
    ctx->urp = pt;
}

void CoreS_SetKernelStack(void* stck)
{
    // TODO: Set kernel stack
    OBOS_UNUSED(stck);
}

void* CoreS_ThreadAlloca(const thread_ctx* ctx_, size_t size, obos_status *status)
{
    thread_ctx* ctx = (void*)ctx_;
    if (!ctx)
    {
        if (status) *status = OBOS_STATUS_INVALID_ARGUMENT;
        return nullptr;
    }
    ctx->usp -= size;
    uintptr_t ret = ctx->usp;
    if (status) *status = OBOS_STATUS_SUCCESS;
    return (void*)ret;
}