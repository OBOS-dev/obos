/*
 * oboskrnl/power/suspend.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>

// NOTE: Only currently supports S3

// NOTE: This thread will always be at IRQL_DISPATCH, even at entry.
// After the architecture reinitializes itself after wake-from-suspend, it should just blindly switch to this thread's context.
// Example:
/*
// do stuff
// NOTE: Since we are on the BSP, which the suspend worker thread is guaranteed to be on, we can just switch to the thread without changing
// anything in the cpu_local struct
OBOS_WokeFromSuspend = true;
CoreS_SwitchToThreadContext(&OBOS_SuspendWorkerThread->ctx);
*/
extern struct thread* OBOS_SuspendWorkerThread;
// NOTE: Set to false automatically.
extern bool OBOS_WokeFromSuspend;

// NOTE: The operation is aborted if there is already someone trying to suspend us.
obos_status OBOS_Suspend();

void OBOS_InitWakeGPEs();

extern uint32_t OBOSS_WakeVector;

extern OBOS_WEAK obos_status OBOSS_PrepareWakeVector();

void OBOSS_SuspendSavePlatformState();

// Saves EC state before entering S3.
void OBOS_ECSave();
// Restores previously saved EC state before entering S3.
void OBOS_ECResume();
