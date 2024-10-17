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
    // scheduler/thread_context_info.h
    HANDLE_TYPE_THREAD_CTX,

    LAST_VALID_HANDLE_TYPE,

    HANDLE_TYPE_ANY = 0xfd,
    HANDLE_TYPE_CURRENT = 0xfe,
    HANDLE_TYPE_INVALID = 0xff,
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
        struct context* vmm_context;
        struct mutex* mutex;
        struct semaphore* semaphore;
        struct pushlock* pushlock;
        struct event* event;
        struct driver_id* driver_id;
        struct thread_ctx_handle* thread_ctx;
        struct waitable_header* waitable;
        void* generic; // just in case
    } un;
} handle_desc;

#define HANDLE_VALUE_MASK (0xffffff)
#define HANDLE_TYPE_SHIFT (24UL)

#define HANDLE_TYPE(hnd) (handle_type)((hnd) >> HANDLE_TYPE_SHIFT)
#define HANDLE_VALUE(hnd) (unsigned int)((hnd) & HANDLE_VALUE_MASK)
typedef uint32_t handle;
#define HANDLE_INVALID (handle)((handle)HANDLE_TYPE_INVALID << HANDLE_TYPE_SHIFT)
#define HANDLE_CURRENT (handle)((handle)HANDLE_TYPE_CURRENT << HANDLE_TYPE_SHIFT)
#define HANDLE_ANY     (handle)((handle)HANDLE_TYPE_ANY     << HANDLE_TYPE_SHIFT)
typedef struct handle_table {
    handle_desc* arr;
    handle_desc* head; // freelist head
    handle last_handle;
    size_t size;
    mutex lock;
} handle_table;

void OBOS_InitializeHandleTable(handle_table* table);
handle_table* OBOS_CurrentHandleTable();
handle_desc* OBOS_HandleLookup(handle_table* table, handle hnd, handle_type type, bool ignoreType, obos_status* status);
handle OBOS_HandleAllocate(handle_table* table, handle_type type, handle_desc** const desc);
void OBOS_HandleFree(handle_table* table, handle_desc *curr);

extern void(*OBOS_HandleCloneCallbacks[LAST_VALID_HANDLE_TYPE])(handle_desc *hnd, handle_desc *new);
extern void(*OBOS_HandleCloseCallbacks[LAST_VALID_HANDLE_TYPE])(handle_desc *hnd);

obos_status Sys_HandleClone(handle hnd, handle* new);
obos_status Sys_HandleClose(handle hnd);
