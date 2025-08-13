/*
 * oboskrnl/locks/wait.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <irq/irql.h>

#include <allocators/base.h>

#include <locks/wait.h>
#include <locks/spinlock.h>

#include <stdarg.h>

obos_status Core_WaitOnObject(struct waitable_header* obj)
{
    if (!obj)
        return OBOS_STATUS_INVALID_ARGUMENT;
    OBOS_ASSERT(Core_GetIrql() <= IRQL_DISPATCH);
    if (Core_GetIrql() > IRQL_DISPATCH)
        return OBOS_STATUS_INVALID_IRQL;
    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    irql spinlockIrql = Core_SpinlockAcquire(&obj->lock);
    if (obj->signaled && obj->use_signaled)
    {
        Core_LowerIrql(oldIrql);
        Core_SpinlockRelease(&obj->lock, spinlockIrql);
        return OBOS_STATUS_SUCCESS;
    }
    thread* curr = Core_GetCurrentThread();
    // We're waiting on one object.
    curr->nSignaled = 0;
    curr->nWaiting = 1;
    memzero(&curr->lock_node, sizeof(curr->lock_node));
    curr->lock_node.data = curr;
    obos_status status = CoreH_ThreadListAppend(&obj->waiting, &curr->lock_node);
    if (obos_is_error(status))
    {
        Core_LowerIrql(oldIrql);
        Core_SpinlockRelease(&obj->lock, spinlockIrql);
        return status;
    }
    Core_SpinlockRelease(&obj->lock, spinlockIrql);
    CoreH_ThreadBlock(curr, true);
    Core_LowerIrql(oldIrql);
    if (curr->interrupted)
    {
        if (curr->signalInterrupted)
        {
            curr->signalInterrupted = false;
            CoreH_ThreadListRemove(&obj->waiting, &curr->lock_node);
        }
        curr->interrupted = false;
        return OBOS_STATUS_ABORTED;
    }
    if (obj->interrupted)
        return OBOS_STATUS_ABORTED;
    return OBOS_STATUS_SUCCESS;
}

obos_status Core_WaitOnObjects(size_t nObjects, struct waitable_header** objs, struct waitable_header** signaled)
{
    if (!nObjects)
        return OBOS_STATUS_INVALID_ARGUMENT;

    OBOS_ASSERT(Core_GetIrql() <= IRQL_DISPATCH);
    if (Core_GetIrql() > IRQL_DISPATCH)
        return OBOS_STATUS_INVALID_IRQL;

    thread* curr = Core_GetCurrentThread();
    curr->nSignaled = 0;
    curr->nWaiting = 1;

    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    
    struct {
        thread_node node;
        struct waitable_header* obj;   
    }* nodes = ZeroAllocate(OBOS_NonPagedPoolAllocator, nObjects, sizeof(*nodes), nullptr);
    for (size_t i = 0; i < nObjects; i++)
    {
        struct waitable_header* const obj = objs[i];
        irql spinlockIrql = Core_SpinlockAcquire(&obj->lock);
        if (obj->signaled)
        {
            if (signaled)
                *signaled = obj;
            Core_SpinlockRelease(&obj->lock, spinlockIrql);
            return OBOS_STATUS_SUCCESS;
        }
        nodes[i].obj = obj;
        nodes[i].node.data = curr;
        CoreH_ThreadListAppend(&obj->waiting, &nodes[i].node);
        Core_SpinlockRelease(&obj->lock, spinlockIrql);
    }

    CoreH_ThreadBlock(curr, true);

    obos_status status = OBOS_STATUS_SUCCESS;

    if (curr->signalInterrupted || curr->hdrSignaled->interrupted)
        status = OBOS_STATUS_ABORTED;

    if (signaled)
        if (!curr->hdrSignaled->interrupted)
            *signaled = Core_GetCurrentThread()->hdrSignaled;
    

    for (size_t i = 0; i < nObjects; i++)
    {
        irql spinlockIrql = Core_SpinlockAcquire(&nodes[i].obj->lock);
        if (nodes[i].obj != curr->hdrSignaled && (curr->interrupted && curr->signalInterrupted))
            CoreH_ThreadListRemove(&nodes[i].obj->waiting, &nodes[i].node);
        Core_SpinlockRelease(&nodes[i].obj->lock, spinlockIrql);
    }

    Core_LowerIrql(oldIrql);

    if (curr->interrupted)
    {
        if (curr->signalInterrupted)
            curr->signalInterrupted = false;
        curr->interrupted = false;
        status = OBOS_STATUS_ABORTED;
    }

    Free(OBOS_NonPagedPoolAllocator, nodes, nObjects * sizeof(*nodes));
    return status;
}

obos_status CoreH_SignalWaitingThreads(struct waitable_header* obj, bool all, bool boostPriority)
{
    if (!obj)
        return OBOS_STATUS_INVALID_ARGUMENT;

    OBOS_ASSERT(Core_GetIrql() <= IRQL_DISPATCH);
    if (Core_GetIrql() > IRQL_DISPATCH)
        return OBOS_STATUS_INVALID_IRQL;

    if (obj->use_signaled)
        obj->signaled = true;

    irql oldIrql = Core_SpinlockAcquire(&obj->lock);
    for (thread_node* curr = obj->waiting.head; curr; )
    {
        thread_node* next = curr->next;
        CoreH_ThreadListRemove(&obj->waiting, curr);
        if (!curr->data)
        {
            curr = next;
            continue;
        }
        if ((++curr->data->nSignaled) == curr->data->nWaiting)
        {
            if (boostPriority)
                CoreH_ThreadBoostPriority(curr->data);
            CoreH_ThreadReadyNode(curr->data, curr->data->snode);
            curr->data->hdrSignaled = obj;
        }
        if (curr->free)
            curr->free(curr);
        if (!all)
            break;
        curr = next;
    }

    Core_SpinlockRelease(&obj->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;   
}

obos_status CoreH_AbortWaitingThreads(struct waitable_header* obj)
{
    if (!obj)
        return OBOS_STATUS_INVALID_ARGUMENT;
    obj->interrupted = 1;
    return CoreH_SignalWaitingThreads(obj, true, false);
}

void CoreH_ClearSignaledState(struct waitable_header* obj)
{
    if (!obj || !obj->use_signaled)
        return;
    irql oldIrql = Core_SpinlockAcquire(&obj->lock);
    obj->signaled = false;
    Core_SpinlockRelease(&obj->lock, oldIrql);
}
