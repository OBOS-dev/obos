/*
 * oboskrnl/locks/semaphore.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <locks/semaphore.h>
#include <locks/spinlock.h>

#include <irq/irql.h>

#include <stdatomic.h>

obos_status Core_SemaphoreAcquire(semaphore* sem)
{
    if (!sem)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irql oldIrql = Core_SpinlockAcquire(&sem->lock);
    if (sem->count)
    {
        sem->count--;
        Core_SpinlockRelease(&sem->lock, oldIrql);
        return OBOS_STATUS_SUCCESS;
    }
    Core_SpinlockRelease(&sem->lock, oldIrql);
    // Add the current thread to the waiting list.
    thread* curr = Core_GetCurrentThread();
    curr->lock_node.data = curr;
    obos_status status = CoreH_ThreadListAppend(&sem->waiting, &curr->lock_node);
    if (obos_is_error(status))
        return status;
    CoreH_ThreadBlock(curr, true);
    oldIrql = Core_SpinlockAcquire(&sem->lock);
    sem->count--;
    Core_SpinlockRelease(&sem->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}
obos_status Core_SemaphoreTryAcquire(semaphore* sem)
{
    if (!sem)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!sem->count)
        return OBOS_STATUS_IN_USE;
    return Core_SemaphoreAcquire(sem);
}
obos_status Core_SemaphoreRelease(semaphore* sem)
{
    // TODO(oberrow): Check if the current thread has the semaphore acquired.
    // NOTE(oberrow): Maybe we don't need to do that.
    if (!sem)
        return OBOS_STATUS_INVALID_ARGUMENT;
    sem->count++;
    // Take a thread from the list, and wake it.
    thread_node* toWake = sem->waiting.head;
    if (toWake)
    {
        irql oldIrql = Core_SpinlockAcquire(&sem->lock);
        CoreH_ThreadListRemove(&sem->waiting, toWake);
        CoreH_ThreadReadyNode(toWake->data, toWake);
        Core_SpinlockRelease(&sem->lock, oldIrql);
    }
    return OBOS_STATUS_SUCCESS;
}
size_t Core_SemaphoreGetValue(semaphore* sem)
{
    return sem->count;
}