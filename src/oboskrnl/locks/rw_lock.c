/*
 * oboskrnk/locks/rw_lock.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <locks/wait.h>
#include <locks/rw_lock.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>

obos_status Core_RwLockAcquire(rw_lock* lock, bool reader /* false: writer, true: reader */)
{
    if (!lock)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (lock->abort)
        return OBOS_STATUS_ABORTED;
    if (reader)
    {
        if (lock->currWriter)
        {
            lock->nWaitingReaders++;
            while(lock->currWriter && !lock->abort)
                OBOSS_SpinlockHint();
            lock->nWaitingReaders--;
        }
        if (lock->abort)
            return OBOS_STATUS_ABORTED;
        lock->nReaders++;
        return OBOS_STATUS_SUCCESS;
    }
    // Hopefully this is right.
    // maybe a possible race condition.
    try_again:
    Core_WaitOnObject(&lock->hdr);
    CoreH_ClearSignaledState(&lock->hdr);
    if (lock->nWaitingReaders)
        goto try_again;
    lock->currWriter = Core_GetCurrentThread();
    return OBOS_STATUS_SUCCESS;
}
obos_status Core_RwLockTryAcquire(rw_lock* lock)
{
    if (!lock)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (lock->nReaders)
        return OBOS_STATUS_IN_USE;
    return Core_RwLockAcquire(lock, false);
}
obos_status Core_RwLockRelease(rw_lock* lock, bool reader)
{
    if (!lock)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (reader)
    {
        if (lock->nReaders)
            lock->nReaders--;
        else
            return OBOS_STATUS_ABORTED; // bruh
        return OBOS_STATUS_SUCCESS;
    }
    lock->currWriter = nullptr;
    return lock->nWaitingReaders ? OBOS_STATUS_SUCCESS : CoreH_SignalWaitingThreads(&lock->hdr, false, true);
}
size_t Core_RwLockGetReaderCount(rw_lock* lock)
{
    if (!lock)
        return 0;
    return lock->nReaders;
}
thread* Core_RwLockGetWriter(const rw_lock* lock)
{
    if (!lock)
        return 0;
    return lock->currWriter;
}
