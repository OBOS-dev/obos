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

obos_status Core_EventPulse(event* event, bool boostWaitingThreadPriority) 
{
    if (!event)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    event->signaled = true;
    if (event->type == EVENT_SYNC)
    {
        thread_node* node = event->waiting.head;
        if (boostWaitingThreadPriority)
            CoreH_ThreadBoostPriority(node->data);
        CoreH_ThreadReadyNode(node->data, node->data->snode);
        CoreH_ThreadListRemove(&event->waiting, node);
        event->signaled = false;
        Core_LowerIrql(oldIrql);
        return OBOS_STATUS_SUCCESS;
    }
    for (thread_node* node = event->waiting.head; node; )
    {
        thread_node* next = node->next;
        if (boostWaitingThreadPriority)
            CoreH_ThreadBoostPriority(node->data);
        CoreH_ThreadListRemove(&event->waiting, node);
        CoreH_ThreadReadyNode(node->data, node->data->snode);
        node = next;
    }
    event->signaled = false;
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
    return OBOS_STATUS_SUCCESS;
}
obos_status Core_EventSet(event* event, bool boostWaitingThreadPriority)
{
    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    event->signaled = true;
    if (event->type == EVENT_SYNC)
    {
        thread_node* node = event->waiting.head;
        if (!node)
            return OBOS_STATUS_SUCCESS;
        if (boostWaitingThreadPriority)
            CoreH_ThreadBoostPriority(node->data);
        CoreH_ThreadReadyNode(node->data, node->data->snode);
        CoreH_ThreadListRemove(&event->waiting, node);
        Core_LowerIrql(oldIrql);
        return OBOS_STATUS_SUCCESS;
    }
    for (thread_node* node = event->waiting.head; node; )
    {
        thread_node* next = node->next;
        if (boostWaitingThreadPriority)
            CoreH_ThreadBoostPriority(node->data);
        CoreH_ThreadListRemove(&event->waiting, node);
        CoreH_ThreadReadyNode(node->data, node->data->snode);
        node = next;
    }
    Core_LowerIrql(oldIrql);
    return OBOS_STATUS_SUCCESS;
}
obos_status Core_EventClear(event* event)
{
    // NOTE(oberrow): Are these functions supposed to be implemented differently?
    return Core_EventReset(event);
}
obos_status Core_EventWait(event* event)
{
    if (!event)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (event->signaled)
        return OBOS_STATUS_SUCCESS;
    Core_GetCurrentThread()->lock_node.data = Core_GetCurrentThread();
    CoreH_ThreadListAppend(&event->waiting, &Core_GetCurrentThread()->lock_node);
    CoreH_ThreadBlock(Core_GetCurrentThread(), true);
    return OBOS_STATUS_SUCCESS;
}