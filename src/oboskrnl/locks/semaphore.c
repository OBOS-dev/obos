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
    CoreH_ThreadListAppend(&sem->waiting, &curr->lock_node);
    CoreH_ThreadBlock(curr, true);
    CoreH_ThreadListRemove(&sem->waiting, &curr->lock_node);
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
    if (!sem)
        return OBOS_STATUS_INVALID_ARGUMENT;
    sem->count++;
    return OBOS_STATUS_SUCCESS;
}
size_t Core_SemaphoreGetValue(semaphore* sem)
{
    return sem->count;
}