/*
	oboskrnl/scheduler/thread_context_info.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <irq/irql.h>

/// <summary>
/// This structure is architecture dependant, but should save at least the following:
/// Thread GPR context.
/// Any floating point (or extended context as it is called on x86_64) if the thread is user mode.
/// IRQL
/// The thread's current address space.
/// </summary>
typedef struct thread_context_info thread_ctx;


// TODO: Add a way to specify the address space of the thread.
/// <summary>
/// Sets up the context of a thread.
/// </summary>
/// <param name="ctx">The thread context to setup.</param>
/// <param name="entry">The entry point of the thread.</param>
/// <param name="arg1">The first argument to be passed to thread's entry.</param>
/// <param name="makeUserMode">Whether the thread should start in user mode or not.</param>
/// <param name="stackBase">The base address of the stack.</param>
/// <param name="stackSize">The size of the stack.</param>
/// <returns>The status code of the function.</returns>
OBOS_WEAK obos_status CoreS_SetupThreadContext(thread_ctx* ctx, uintptr_t entry, uintptr_t arg1, bool makeUserMode, void* stackBase, size_t stackSize);
/// <summary>
/// Switches to a different thread's context.
/// </summary>
/// <param name="ctx">The thread's context.</param>
OBOS_NORETURN OBOS_WEAK void CoreS_SwitchToThreadContext(const thread_ctx* ctx);
/// <summary>
/// Saves the current thread's context into the passed thread's context, then calls the scheduler.
/// </summary>
/// <param name="ctx">The thread's context.</param>
OBOS_WEAK void CoreS_SaveRegisterContextAndYield(thread_ctx* ctx);
/// <summary>
/// Frees anything inside of a thread's context.
/// </summary>
/// <param name="ctx">The thread's context.</param>
/// <returns>The status code of the function.</returns>
OBOS_WEAK obos_status CoreS_FreeThreadContext(thread_ctx* ctx);
/// <summary>
/// Calls a function on a stack allocated in the kernel address space.<para/>
/// This function must be thread-safe. It mustn't use the same stack as another CPU or thread.<para/>
/// This function is intended to be used in contexts such as ExitCurrentThread, where it might free the stack.
/// </summary>
/// <param name="func">The function to be called.</param>
/// <param name="userdata">The parameter to be passed to the function.</param>
/// <returns>The callback's return value.</returns>
OBOS_WEAK uintptr_t CoreS_CallFunctionOnStack(uintptr_t(*func)(uintptr_t), uintptr_t userdata);

/// <summary>
/// Sets the IRQL of a thread. This function should be infallible if the parameters are correct.
/// </summary>
/// <param name="ctx">The thread's context.</param>
/// <param name="newIRQL">The new IRQL of the thread.</param>
OBOS_WEAK void CoreS_SetThreadIRQL(thread_ctx* ctx, irql newIRQL);
/// <summary>
/// Gets the IRQL of a thread. This function should be infallible if the parameters are correct.
/// </summary>
/// <param name="ctx">The thread's context.</param>
/// <returns>The thread's IRQL.</returns>
OBOS_WEAK irql CoreS_GetThreadIRQL(const thread_ctx* ctx);
/// <summary>
/// Gets the base of the stack of a thread.
/// </summary>
/// <param name="ctx">The thread's context.</param>
/// <returns>The thread's stack.</returns>
OBOS_WEAK void* CoreS_GetThreadStack(const thread_ctx* ctx);
/// <summary>
/// Gets the size of the stack of a thread.
/// </summary>
/// <param name="ctx">The thread's context.</param>
/// <returns>The thread's stack size.</returns>
OBOS_WEAK size_t CoreS_GetThreadStackSize(const thread_ctx* ctx);

#ifdef __x86_64__
#	include <arch/x86_64/thread_ctx.h>
#elif defined(__m68k__)
#	include <arch/m68k/thread_ctx.h>
#endif

// Some helpers to free stacks allocated using the VMA and the basic MM.

// userdata should be the context* used to allocate the stack.
OBOS_EXPORT void CoreH_VMAStackFree(void* base, size_t sz, void* userdata);
// userdata is unused.
void CoreH_BasicMMStackFree(void* base, size_t sz, void* userdata);