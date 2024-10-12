/*
 * oboskrnk/locks/pushlock.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <locks/wait.h>
#include <locks/pushlock.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>

obos_status Core_PushlockAcquire(pushlock* lock, bool reader /* false: writer, true: reader */)
{
    if (!lock)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (reader)
    {
        if (lock->currWriter)
        {
            lock->nWaitingReaders++;
            while(lock->currWriter);
            lock->nWaitingReaders--;
        }
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
obos_status Core_PushlockTryAcquire(pushlock* lock)
{
    if (!lock)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (lock->nReaders)
        return OBOS_STATUS_IN_USE;
    return Core_PushlockAcquire(lock, false);
}
obos_status Core_PushlockRelease(pushlock* lock, bool reader)
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
size_t Core_PushlockGetReaderCount(pushlock* lock)
{
    if (!lock)
        return 0;
    return lock->nReaders;
}
thread* Core_PushlockGetWriter(const pushlock* lock)
{
    if (!lock)
        return 0;
    return lock->currWriter;
}
