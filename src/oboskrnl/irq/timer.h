/*
 * oboskrnl/irq/timer.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <irq/irql.h>
#include <irq/irq.h>

typedef uint64_t timer_tick;
typedef uint64_t timer_frequency;

extern struct irq* Core_TimerIRQ;
extern timer_frequency CoreS_TimerFrequency;
extern bool Core_TimerInterfaceInitialized;

/// <summary>
/// Handles a timer.
/// </summary>
/// <param name="t">The timer object.</param>
/// <param name="userdata">The user data.</param>
typedef void(*timer_handler)(void* userdata);
typedef enum {
    TIMER_UNINITIALIZED,
    TIMER_EXPIRED,
    TIMER_MODE_DEADLINE,
    TIMER_MODE_INTERVAL,
} timer_mode;
typedef struct timer {
    union {
        timer_tick deadline;
        timer_tick interval;
    } timing;
    timer_tick lastTimeTicked;
    timer_mode mode;
    timer_handler handler;
    void* userdata;
    struct timer* next;
    struct timer* prev;
} timer;

/// <summary>
/// Initializes the timer interface.
/// </summary>
/// <returns>The status of the function.</returns>
obos_status Core_InitializeTimerInterface();
/// <summary>
/// Allocates (but doesn't construct) a timer object.
/// </summary>
/// <param name="status">[out, optional] The status of the function.</param>
/// <returns>The object, or nullptr on failure.</returns>
OBOS_EXPORT timer* Core_TimerObjectAllocate(obos_status* status);
/// <summary>
/// Frees a timer object allocated with Core_TimerObjectAllocate.<para></para>
/// The timer must be cancelled, or uninitialized.
/// </summary>
/// <param name="obj">The timer to free.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status Core_TimerObjectFree(timer* obj);
/// <summary>
/// Registers a timer object.
/// </summary>
/// <param name="obj">The timer object.</param>
/// <param name="mode">The timer's mode.</param>
/// <param name="period">The period of time in microseconds the timer shall run on.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status Core_TimerObjectInitialize(timer* obj, timer_mode mode, uint64_t period);
/// <summary>
/// Cancels a timer.
/// </summary>
/// <param name="obj">The timer object.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status Core_CancelTimer(timer* obj);

/// <summary>
/// Gets the current tick.
/// </summary>
/// <returns>The current timer tick.</returns>
OBOS_EXPORT timer_tick CoreS_GetTimerTick();
/// <summary>
/// Converts a time frame in us to timer ticks.
/// </summary>
/// <param name="us">The time frame in microseconds.</param>
/// <returns>The object, or nullptr on failure.</returns>
OBOS_EXPORT timer_tick CoreH_TimeFrameToTick(uint64_t us);

/// <summary>
/// Initializes the timer irq (should already be allocated) and the timer.
/// </summary>
/// <param name="handler">The irq handler.</param>
/// <returns>The status of the function.</returns>
OBOS_WEAK obos_status CoreS_InitializeTimer(irq_handler handler);
#ifdef OBOS_TIMER_IS_DEADLINE
obos_status CoreS_ResetTimer();
#endif