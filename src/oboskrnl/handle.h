/*
 * oboskrnl/handle.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>

#include <locks/mutex.h>

typedef enum handle_type
{
    // vfs/fd.h
    HANDLE_TYPE_FD,
    // irq/timer.h
    HANDLE_TYPE_TIMER,
    // vfs/dirent.h
    HANDLE_TYPE_DIRENT,
    // scheduler/thread.h
    HANDLE_TYPE_THREAD,
    // scheduler/process.h
    HANDLE_TYPE_PROCESS,
    // mm/context.h
    HANDLE_TYPE_VMM_CONTEXT,
    // locks/mutex.h
    HANDLE_TYPE_MUTEX,
    // locks/semaphore.h
    HANDLE_TYPE_SEMAPHORE,
    // locks/pushlock.h
    HANDLE_TYPE_PUSHLOCK,
    // locks/event.h
    HANDLE_TYPE_EVENT,
    // driver_interface/driverId.h
    HANDLE_TYPE_DRIVER_ID,
} handle_type;
typedef struct handle_desc
{
    union {
        struct handle_desc* next; // for the freelist.
        struct fd* fd;
        struct timer* timer;
        struct dirent* dirent;
        struct thread* thread;
        struct process* process;
        struct context* context;
        struct mutex* mutex;
        struct semaphore* semaphore;
        struct pushlock* pushlock;
        struct event* event;
        struct driver_id* driver_id;
        void* generic; // just in case
    } un;
} handle_desc;
typedef unsigned int handle;
typedef struct handle_table {
    handle_desc* arr;
    handle_desc* head; // freelist head
    handle last_handle;
    size_t size;
    mutex lock;
} handle_table;

void OBOS_InitializeHandleTable(handle_table* table);
handle_desc* OBOS_HandleLookup(handle_table* table, handle hnd, handle_type type);
handle OBOS_HandleAllocate(handle_table* table, handle_type type, handle_desc** const desc);
void OBOS_HandleFree(handle_table* table, handle hnd);
