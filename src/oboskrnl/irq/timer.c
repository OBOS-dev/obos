/*
 * oboskrnl/irq/timer.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include "utils/list.h"
#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>
#include <text.h>

#include <irq/irql.h>
#include <irq/irq.h>
#include <irq/timer.h>

#include <allocators/base.h>

#include <mm/bare_map.h>

#include <external/fixedptc.h>

#include <locks/spinlock.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>
#include <scheduler/cpu_local.h>
#include <scheduler/thread_context_info.h>
#include <scheduler/dpc.h>

bool Core_TimerInterfaceInitialized;
irq* Core_TimerIRQ;
static struct
{
    timer* head;
    timer* tail;
    size_t nNodes;
    spinlock lock;
} timer_list;
static dpc* timer_dispatcher(dpc* obj, void* userdata);
dpc* work = nullptr;
OBOS_NO_KASAN OBOS_NO_UBSAN static void timer_irq(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(frame);
    OBOS_UNUSED(userdata);
    OBOS_UNUSED(oldIrql);
    if (!work->cpu || LIST_IS_NODE_UNLINKED(dpc_queue, &work->cpu->dpcs, work))
        CoreH_InitializeDPC(work, timer_dispatcher, Core_DefaultThreadAffinity);
    OBOS_Debug("Received timer IRQ.\n");
}
static void notify_timer(timer* timer)
{
    // TODO: Use signals instead of calling the handler directly.
    timer->lastTimeTicked = CoreS_GetTimerTick();
    if (timer->mode == TIMER_MODE_DEADLINE)
        Core_CancelTimer(timer);
    timer->handler(timer->userdata);
}
static dpc* timer_dispatcher(dpc* obj, void* userdata)
{
    OBOS_UNUSED(userdata);
    // Search for expired timer objects, and notify them.
    for(timer* t = timer_list.head; t; )
    {
        bool expired = false;
        switch (t->mode) 
        {
            case TIMER_MODE_DEADLINE:
            {
                expired = CoreS_GetTimerTick() >= t->timing.deadline;
                break;
            }
            case TIMER_MODE_INTERVAL:
            {
                timer_tick deadline = t->timing.interval + t->lastTimeTicked;
                expired = CoreS_GetTimerTick() >= deadline;
                break;
            }
            default:
                break;
        }
        if (!expired)
            goto end;
        notify_timer(t);
        end:
        (void)0;
        irql oldIrql = Core_SpinlockAcquireExplicit(&timer_list.lock, IRQL_TIMER, false);
        t = t->next;
        Core_SpinlockRelease(&timer_list.lock, oldIrql);
    }
    return obj;
}
obos_status Core_InitializeTimerInterface()
{
    irql oldIrql = Core_RaiseIrql(IRQL_TIMER);
    obos_status status = OBOS_STATUS_SUCCESS;
    Core_TimerIRQ = Core_IrqObjectAllocate(&status);
    if (obos_is_error(status))
        goto cleanup1;
    status = CoreS_InitializeTimer(timer_irq);
    if (obos_is_error(status))
        goto cleanup1;
    Core_TimerInterfaceInitialized = true;
    cleanup1:
    if (obos_is_error(status))
        if (Core_TimerIRQ)
            Core_IrqObjectFree(Core_TimerIRQ);
    work = CoreH_AllocateDPC(nullptr);
    Core_LowerIrql(oldIrql);
    return status;
}
timer* Core_TimerObjectAllocate(obos_status* status)
{
    if (!OBOS_NonPagedPoolAllocator)
		return OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(process), status);
	return OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(process), status);
}
obos_status Core_TimerObjectFree(timer* obj)
{
    if (!obj)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (obj->mode > TIMER_EXPIRED)
        return OBOS_STATUS_ACCESS_DENIED;
    return OBOS_KernelAllocator->Free(OBOS_KernelAllocator, obj, sizeof(*obj));
}
obos_status Core_TimerObjectInitialize(timer* obj, timer_mode mode, uint64_t us)
{
    if (!obj || !us || mode < TIMER_EXPIRED)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    timer_tick ticks = CoreH_TimeFrameToTick(us);
    switch (mode) 
    {
        case TIMER_MODE_DEADLINE:
            obj->timing.deadline = obj->lastTimeTicked + ticks;
            break;
        case TIMER_MODE_INTERVAL:
            obj->timing.interval = ticks;
            break;
        default:
            Core_LowerIrql(oldIrql);
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    obj->lastTimeTicked = CoreS_GetTimerTick();
    obj->mode = mode;
    irql oldIrql2 = Core_SpinlockAcquireExplicit(&timer_list.lock, IRQL_TIMER, false);
    if (timer_list.tail)
        timer_list.tail->next = obj;
    if (!timer_list.head)
        timer_list.head = obj;
    obj->prev = timer_list.tail;
    timer_list.tail = obj;
    timer_list.nNodes++;
    Core_SpinlockRelease(&timer_list.lock, oldIrql2);
    Core_LowerIrql(oldIrql);
    return OBOS_STATUS_SUCCESS;
}
obos_status Core_CancelTimer(timer* timer)
{
    if (!timer)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (timer->mode == TIMER_UNINITIALIZED)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (timer->mode == TIMER_EXPIRED)
        return OBOS_STATUS_SUCCESS;
    irql oldIrql = Core_SpinlockAcquireExplicit(&timer_list.lock, IRQL_TIMER, false);
    // Remove the timer from the list.
    if (timer->next)
        timer->next->prev = timer->prev;
    if (timer->prev)
        timer->prev->next = timer->next;
    if (timer == timer_list.head)
        timer_list.head = timer->next;
    if (timer == timer_list.tail)
        timer_list.tail = timer->prev;
    timer_list.nNodes--;
    Core_SpinlockRelease(&timer_list.lock, oldIrql);
    timer->mode = TIMER_EXPIRED;
    return OBOS_STATUS_SUCCESS;
    
}
timer_tick CoreH_TimeFrameToTick(uint64_t us)
{
    // us/1000000*freqHz=timer ticks
    fixedptd tp = fixedpt_fromint(us); // us.0
    fixedptd hz = fixedpt_fromint(CoreS_TimerFrequency); // CoreS_TimerFrequency.0
    const fixedptd divisor = fixedpt_fromint(1000000); // 1000000.0
    OBOS_ASSERT(fixedpt_toint(tp) == (int64_t)us);
    OBOS_ASSERT(fixedpt_toint(hz) == (int64_t)CoreS_TimerFrequency);
    OBOS_ASSERT(fixedpt_toint(divisor) == 1000000);
    tp = fixedpt_xdiv(tp, divisor);
    tp = fixedpt_xmul(tp, hz);
    return fixedpt_toint(tp)+1 /* add one to account for rounding issues. */;
}