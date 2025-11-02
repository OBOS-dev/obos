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

#if OBOS_DEBUG
#   define assert_initialized() OBOS_ASSERT(obj->initialized == true)
#else
#   define assert_initialized() (true)
#endif

obos_status Core_WaitOnObject(struct waitable_header* obj)
{
    if (!obj)
        return OBOS_STATUS_INVALID_ARGUMENT;
    OBOS_ASSERT(Core_GetIrql() <= IRQL_DISPATCH);
    assert_initialized();
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
    curr->waiting_objects.waitingObject = obj;
    curr->waiting_objects.is_array = false;
    curr->waiting_objects.free_array = nullptr;
    curr->waiting_objects.free_userdata = nullptr;
    curr->waiting_objects.nObjs = 1;
    Core_SpinlockRelease(&obj->lock, spinlockIrql);
    CoreH_ThreadBlock(curr, true);
    memzero(&curr->waiting_objects, sizeof(curr->waiting_objects));
    if (curr->interrupted)
    {
        if (curr->signalInterrupted)
        {
            curr->signalInterrupted = false;
            CoreH_ThreadListRemove(&obj->waiting, &curr->lock_node);
#ifdef __x86_64__
            Core_LowerIrql(oldIrql);
            asm volatile("hlt" :::"memory");
            oldIrql = IRQL_INVALID;
#endif
        }
        curr->interrupted = false;
        if (oldIrql != IRQL_INVALID)
            Core_LowerIrql(oldIrql);
        Core_Yield();
        return OBOS_STATUS_ABORTED;
    }
    CoreH_ThreadListRemove(&obj->waiting, &curr->lock_node);
    curr->hdrSignaled = nullptr;
    curr->nWaiting = 0;
    Core_LowerIrql(oldIrql);
    Core_Yield();
    if (obj->interrupted)
        return OBOS_STATUS_ABORTED;
    return OBOS_STATUS_SUCCESS;
}

static void free_arr(void* udata, struct waiting_array_node* objs, size_t nObjs)
{
    ((allocator_info*)udata)->Free(udata, objs, nObjs*sizeof(struct waiting_array_node*));
}

obos_status Core_WaitOnObjects(size_t nObjects, struct waitable_header** objs, struct waitable_header** signaled)
{
    if (!nObjects)
        return OBOS_STATUS_INVALID_ARGUMENT;

    OBOS_ASSERT(Core_GetIrql() <= IRQL_DISPATCH);
    if (Core_GetIrql() > IRQL_DISPATCH)
        return OBOS_STATUS_INVALID_IRQL;

    thread* curr = Core_GetCurrentThread();
    curr->nWaiting = 1;

    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    
    waiting_array_node* nodes = ZeroAllocate(OBOS_NonPagedPoolAllocator, nObjects, sizeof(*nodes), nullptr);
    for (size_t i = 0; i < nObjects; i++)
    {
        struct waitable_header* const obj = objs[i];
        assert_initialized();
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
    curr->waiting_objects.is_array = true;
    curr->waiting_objects.free_array = free_arr;
    curr->waiting_objects.free_userdata = OBOS_NonPagedPoolAllocator;
    curr->waiting_objects.waitingObjects = nodes;
    curr->waiting_objects.nObjs = nObjects;

    CoreH_ThreadBlock(curr, true);
    memzero(&curr->waiting_objects, sizeof(curr->waiting_objects));

    obos_status status = OBOS_STATUS_SUCCESS;

    if (curr->signalInterrupted || (curr->hdrSignaled && curr->hdrSignaled->interrupted))
        status = OBOS_STATUS_ABORTED;

    if (signaled)
    {
        if (curr->hdrSignaled && !curr->hdrSignaled->interrupted)
            *signaled = Core_GetCurrentThread()->hdrSignaled;
        else
            *signaled = nullptr;
    }

    for (size_t i = 0; i < nObjects; i++)
    {
        irql spinlockIrql = Core_SpinlockAcquire(&nodes[i].obj->lock);
        CoreH_ThreadListRemove(&nodes[i].obj->waiting, &nodes[i].node);
        Core_SpinlockRelease(&nodes[i].obj->lock, spinlockIrql);
    }

    curr->hdrSignaled = nullptr;
    curr->nWaiting = 0;

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
    assert_initialized();

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
        if (boostPriority)
            CoreH_ThreadBoostPriority(curr->data);
        CoreH_ThreadReady(curr->data);
        curr->data->hdrSignaled = obj;
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
    assert_initialized();
    obj->interrupted = 1;
    return CoreH_SignalWaitingThreads(obj, true, false);
}

void CoreH_ClearSignaledState(struct waitable_header* obj)
{
    if (!obj || !obj->use_signaled)
        return;
    assert_initialized();
    irql oldIrql = Core_SpinlockAcquire(&obj->lock);
    obj->signaled = false;
    Core_SpinlockRelease(&obj->lock, oldIrql);
}
