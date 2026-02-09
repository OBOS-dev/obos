/*
 * oboskrnl/irq/timer.c
 * 
 * Copyright (c) 2024-2026 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>
#include <text.h>

#include <irq/irql.h>
#include <irq/irq.h>
#include <irq/timer.h>
#include <irq/dpc.h>

#include <locks/event.h>

#include <allocators/base.h>

#include <mm/bare_map.h>

#include <external/fixedptc.h>

#include <locks/spinlock.h>

#include <utils/list.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>
#include <scheduler/cpu_local.h>
#include <scheduler/thread_context_info.h>

bool Core_TimerInterfaceInitialized;
irq* Core_TimerIRQ;
static struct
{
    timer* head;
    timer* tail;
    size_t nNodes;
    spinlock lock;
} timer_list;
static void timer_dispatcher(dpc* obj, void* userdata);
dpc* work = nullptr;
OBOS_NO_KASAN OBOS_NO_UBSAN static void timer_irq(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(frame);
    OBOS_UNUSED(userdata);
    OBOS_UNUSED(oldIrql);
#ifdef OBOS_TIMER_IS_DEADLINE
    CoreS_ResetTimer();
#endif
    if (!work->cpu || LIST_IS_NODE_UNLINKED(dpc_queue, &work->cpu->dpcs, work))
        CoreH_InitializeDPC(work, timer_dispatcher, CoreH_CPUIdToAffinity(CoreS_GetCPULocalPtr()->id));
}
static void notify_timer_dpc(dpc* dpc, void* userdata)
{
    OBOS_UNUSED(dpc);
    timer* timer = userdata;
    if (timer)
        timer->handler(timer->userdata);
}
static void notify_timer(timer* timer)
{
    // TODO: Use signals instead of calling the handler directly.
    timer->lastTimeTicked = CoreS_GetTimerTick();
    if (timer->mode == TIMER_MODE_DEADLINE)
        Core_CancelTimer(timer);
    timer->handler_dpc.userdata = timer;
    CoreH_InitializeDPC(&timer->handler_dpc, notify_timer_dpc, Core_DefaultThreadAffinity);
    // timer->handler(timer->userdata);
}
static void timer_dispatcher(dpc* obj, void* userdata)
{
    OBOS_UNUSED(obj);
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
        OBOS_ASSERT(t->mode != TIMER_EXPIRED);
        notify_timer(t);
        end:
        (void)0;
        irql oldIrql = Core_SpinlockAcquireExplicit(&timer_list.lock, IRQL_TIMER, false);
        t = t->next;
        Core_SpinlockRelease(&timer_list.lock, oldIrql);
    }
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
		return ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(timer), status);
	return ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(timer), status);
}
obos_status Core_TimerObjectFree(timer* obj)
{
    if (!obj)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (obj->mode > TIMER_EXPIRED)
        return OBOS_STATUS_ACCESS_DENIED;
    CoreH_FreeDPC(&obj->handler_dpc, false);
    return Free(OBOS_KernelAllocator, obj, sizeof(*obj));
}
obos_status Core_TimerObjectInitialize(timer* obj, timer_mode mode, uint64_t us)
{
    if (!obj || !us || mode < TIMER_EXPIRED)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    timer_tick ticks = CoreH_TimeFrameToTick(us);
    obj->lastTimeTicked = CoreS_GetTimerTick();
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
    CoreH_FreeDPC(&timer->handler_dpc, false);
    Core_SpinlockRelease(&timer_list.lock, oldIrql);
    timer->mode = TIMER_EXPIRED;
    return OBOS_STATUS_SUCCESS;
    
}
timer_tick CoreH_TimeFrameToTick(uint64_t us)
{
    // us/1000=timer ticks
// #if OBOS_ARCH_USES_SOFT_FLOAT
//     return ((double)us/1000.0*(double)CoreS_TimerFrequency)+1;
// #else
    fixedptd tp = fixedpt_fromint(us); // us.0
    const fixedptd divisor = fixedpt_fromint(CoreS_TimerFrequency); // 1000.0
    OBOS_ASSERT(fixedpt_toint(tp) == (int64_t)us);
    OBOS_ASSERT(fixedpt_toint(divisor) == 1000);
    tp = fixedpt_xdiv(tp, divisor);
    return fixedpt_toint(tp)+1 /* add one to account for rounding issues. */;
// #endif
}

OBOS_EXPORT uint64_t CoreH_TickToNS(timer_tick tick, bool native_tick)
{
    if (native_tick)
    {
        static uint64_t cached_rate = 0;
        // NOTE: If our frequency is greater than 1 GHZ, we get zero for our rate.
        if (obos_expect(!cached_rate, false))
        {
            cached_rate = (1*1000000000)/CoreS_GetNativeTimerFrequency();
            if (!cached_rate)
                OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Conversion from a native timer tick to NS failed.\nNative timer frequency was greater than 1GHZ, which is unsupported. This is a bug, report it.\n");
        }
        return tick * cached_rate;
    }
    else
    {
        static uint64_t cached_rate = 0;
        // NOTE: If our frequency is greater than 1 GHZ, we get zero for our rate.
        if (obos_expect(!cached_rate, false))
        {
            cached_rate = (1*1000000000)/CoreS_TimerFrequency;
            if (!cached_rate)
                OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Conversion from a timer tick to NS failed.\nTimer frequency was greater than 1GHZ, which is unsupported. This is a bug, report it.\n");
        }
        return tick * cached_rate;
    }
}

static void tm_evnt_hnd(void* udata)
{
    event* evnt = udata;
    Core_EventSet(evnt, false);
}

void CoreH_MakeTimerEvent(timer** otm, uint64_t us, event* evnt, bool recurring)
{
    OBOS_ASSERT(otm);
    *otm = Core_TimerObjectAllocate(nullptr);
    (*otm)->handler = tm_evnt_hnd;
    (*otm)->userdata = (void*)evnt;
    Core_TimerObjectInitialize(*otm, recurring ? TIMER_MODE_INTERVAL : TIMER_MODE_DEADLINE, us);
}