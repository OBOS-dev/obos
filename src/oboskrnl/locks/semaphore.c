/*
 * oboskrnl/locks/semaphore.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <locks/semaphore.h>
#include <locks/spinlock.h>
#include <locks/wait.h>

#include <irq/irql.h>

#include <stdatomic.h>

obos_status Core_SemaphoreAcquire(semaphore* sem)
{
    if (!sem)
        return OBOS_STATUS_INVALID_ARGUMENT;
    OBOS_ASSERT(Core_GetIrql() <= IRQL_DISPATCH);
    if (Core_GetIrql() > IRQL_DISPATCH)
        return OBOS_STATUS_INVALID_IRQL;
    irql oldIrql = Core_SpinlockAcquire(&sem->lock);
    if (sem->count)
    {
        sem->count--;
        Core_SpinlockRelease(&sem->lock, oldIrql);
        return OBOS_STATUS_SUCCESS;
    }
    Core_SpinlockRelease(&sem->lock, oldIrql);
    // Add the current thread to the waiting list.
    obos_status status = Core_WaitOnObject(&sem->hdr);
    if (obos_is_error(status))
        return status;
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
    if (!sem)
        return OBOS_STATUS_INVALID_ARGUMENT;
    sem->count++;
    CoreH_SignalWaitingThreads(&sem->hdr, false, false);
    CoreH_ClearSignaledState(&sem->hdr);
    return OBOS_STATUS_SUCCESS;
}
size_t Core_SemaphoreGetValue(semaphore* sem)
{
    return sem->count;
}