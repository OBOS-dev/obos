/*
 * oboskrnl/locks/event.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <stdatomic.h>

#include <irq/irql.h>

#include <locks/event.h>
#include <locks/wait.h>

obos_status Core_EventPulse(event* event, bool boostWaitingThreadPriority) 
{
    if (!event)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    event->signaled = true;
    CoreH_SignalWaitingThreads((struct waitable_header*)&event->hdr, !(event->type == EVENT_SYNC), boostWaitingThreadPriority);
    event->signaled = false;
    CoreH_ClearSignaledState((struct waitable_header*)&event->hdr);
    Core_LowerIrql(oldIrql);
    return OBOS_STATUS_SUCCESS;
}
bool Core_EventGetState(const event* event)
{
    if (!event)
        return false;
    return event->signaled;
}
obos_status Core_EventReset(event* event)
{
    if (!event)
        return OBOS_STATUS_INVALID_ARGUMENT;
    event->signaled = false;
    CoreH_ClearSignaledState((struct waitable_header*)&event->hdr);
    return OBOS_STATUS_SUCCESS;
}
obos_status Core_EventSet(event* event, bool boostWaitingThreadPriority)
{
    if (!event)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    CoreH_SignalWaitingThreads((struct waitable_header*)&event->hdr, !(event->type == EVENT_SYNC), boostWaitingThreadPriority);
    event->signaled = true;
    // Core_LowerIrql(oldIrql);
    return OBOS_STATUS_SUCCESS;
}
obos_status Core_EventClear(event* event)
{
    // NOTE(oberrow): Are these functions supposed to be implemented differently?
    return Core_EventReset(event);
}