/*
 * oboskrnl/locks/wait.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>

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
    if (obj->signaled && obj->use_signaled)
        return OBOS_STATUS_SUCCESS;
    thread* curr = Core_GetCurrentThread();
    // We're waiting on one object.
    curr->nSignaled = 0;
    curr->nWaiting = 1;
    curr->lock_node.data = curr;
    obos_status status = CoreH_ThreadListAppend(&obj->waiting, &curr->lock_node);
    if (obos_is_error(status))
        return status;
    if (obj->signaled && obj->use_signaled)
        return OBOS_STATUS_SUCCESS; // there is a possibility that a dpc ran which signalled the objectq.
    CoreH_ThreadBlock(curr, true);
    return OBOS_STATUS_SUCCESS;
}
static void free_node(thread_node* n)
{
    OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, n, sizeof(*n));
}
obos_status Core_WaitOnObjects(size_t nObjects, ...)
{
    if (!nObjects)
        return OBOS_STATUS_INVALID_ARGUMENT;
    va_list list;
    va_start(list, nObjects);
    thread* curr = Core_GetCurrentThread();
    curr->lock_node.data = curr;
    curr->nSignaled = 0;
    curr->nWaiting = 0;
    for (size_t i = 0; i < nObjects; i++)
    {
        struct waitable_header* obj = va_arg(list, struct waitable_header*);
        if (obj->signaled && obj->use_signaled)
            continue;
        thread_node* node = OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(thread_node), nullptr);
        node->data = curr;
        node->free = free_node;
        obos_status status = CoreH_ThreadListAppend(&obj->waiting, node);
        if (obos_is_error(status))
            continue;
        curr->nWaiting++;
    }
    va_end(list);
    if (curr->nWaiting)
        CoreH_ThreadBlock(curr, true);
    return OBOS_STATUS_SUCCESS;
}
obos_status Core_WaitOnObjectsPtr(size_t nObjects, size_t stride, struct waitable_header* objs)
{
    if (!nObjects)
        return OBOS_STATUS_INVALID_ARGUMENT;
    thread* curr = Core_GetCurrentThread();
    curr->nSignaled = 0;
    curr->nWaiting = 0;
    for (size_t i = 0; i < nObjects; i++)
    {
        struct waitable_header* obj = (struct waitable_header*)((uintptr_t)objs + stride*i);
        if (obj->signaled && obj->use_signaled)
            continue;
        thread_node* node = OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(thread_node), nullptr);
        node->data = curr;
        node->free = free_node;
        obos_status status = CoreH_ThreadListAppend(&obj->waiting, node);
        if (obos_is_error(status))
            continue;
        curr->nWaiting++;
    }
    if (curr->nWaiting)
        CoreH_ThreadBlock(curr, true);
    return OBOS_STATUS_SUCCESS;
}
obos_status CoreH_SignalWaitingThreads(struct waitable_header* obj, bool all, bool boostPriority)
{
    if (!obj)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (obj->use_signaled)
        obj->signaled = true;
    for (thread_node* curr = obj->waiting.head; curr; )
    {
        thread_node* next = curr->next;
        CoreH_ThreadListRemove(&obj->waiting, curr);
        if ((++curr->data->nSignaled) == curr->data->nWaiting)
        {
            if (boostPriority)
                CoreH_ThreadBoostPriority(curr->data);
            CoreH_ThreadReadyNode(curr->data, curr->data->snode);
        }
        if (curr->free)
            curr->free(curr);
        if (!all)
            break;
        curr = next;
    }
    return OBOS_STATUS_SUCCESS;   
}
void CoreH_ClearSignaledState(struct waitable_header* obj)
{
    if (!obj || !obj->use_signaled)
        return;
    obj->signaled = false;
}