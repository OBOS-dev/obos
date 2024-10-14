/*
 * oboskrnl/handle.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <handle.h>

#include <scheduler/cpu_local.h>
#include <scheduler/process.h>

#include <locks/mutex.h>

#include <allocators/base.h>

void expand_handle_table(handle_table* table, size_t size)
{
    const size_t oldSize= table->size;
    table->size = size;
    table->arr = OBOS_KernelAllocator->Reallocate(OBOS_KernelAllocator, table->arr, sizeof(*table->arr)*table->size, nullptr);
    memzero(table->arr + oldSize, sizeof(handle_desc)*(table->size-oldSize));
    table->last_handle = oldSize;
}
void OBOS_InitializeHandleTable(handle_table* table)
{
    table->lock = MUTEX_INITIALIZE();
    expand_handle_table(table, 64);
}
#define in_range(ra,rb,x) (((x) >= (ra)) && ((x) < (rb)))
handle_desc* OBOS_HandleLookup(handle_table* table, handle hnd, handle_type type, bool ignoreType, obos_status* status)
{
    OBOS_ASSERT(table);
    // First, validate the handle type.
    if (HANDLE_TYPE(hnd) >= LAST_VALID_HANDLE_TYPE)
    {
        if (status)
            *status = OBOS_STATUS_INVALID_ARGUMENT;
        return nullptr;
    }
    if (HANDLE_TYPE(hnd) != type && !ignoreType)
    {
        if (status)
            *status = OBOS_STATUS_INVALID_ARGUMENT;
        return nullptr;
    }
    hnd &= HANDLE_VALUE_MASK;
    if (hnd >= table->size)
    {
        if (status)
            *status = OBOS_STATUS_INVALID_ARGUMENT;
        return nullptr;
    }
    if (in_range(table->arr, table->arr+table->size, table->arr[hnd].un.next))
    {
        if (status)
            *status = OBOS_STATUS_INVALID_ARGUMENT;
        return nullptr; // use-after-free
    }
    if (!table->arr[hnd].un.next)
    {
        if (status)
            *status = OBOS_STATUS_INVALID_ARGUMENT;
        return nullptr; // use-after-free
    }
    *status = OBOS_STATUS_SUCCESS;
    return &table->arr[hnd];
}
handle OBOS_HandleAllocate(handle_table* table, handle_type type, handle_desc** const desc)
{
    OBOS_ASSERT(table);
    OBOS_ASSERT(desc);
    handle hnd = 0;
    Core_MutexAcquire(&table->lock);
    if (table->head)
    {
        hnd = table->head - table->arr;
        table->head = table->head->un.next;
    }
    else
    {
        hnd = table->last_handle++;
        if (hnd >= table->size)
            expand_handle_table(table, OBOS_MAX(table->size + (table->size / 4), hnd));
    }
    *desc = &table->arr[hnd];
    hnd |= (type << HANDLE_TYPE_SHIFT);
    Core_MutexRelease(&table->lock);
    return hnd;
}
void OBOS_HandleFree(handle_table* table, handle_desc *curr)
{
    Core_MutexAcquire(&table->lock);
    curr->un.next = table->head;
    table->head = curr;
    Core_MutexRelease(&table->lock);
    // any use of this handle past here is a use-after-free
}

void(*OBOS_HandleCloneCallbacks[LAST_VALID_HANDLE_TYPE])(handle_desc *hnd, handle_desc *new);
void(*OBOS_HandleCloseCallbacks[LAST_VALID_HANDLE_TYPE])(handle_desc *hnd);

obos_status Sys_HandleClone(handle hnd, handle* unew)
{
    handle_table* const current_table = &CoreS_GetCPULocalPtr()->currentThread->proc->handles;

    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* desc = OBOS_HandleLookup(current_table, hnd, 0, true, &status);
    if (obos_is_error(status))
        return status;

    handle_type type = HANDLE_TYPE(hnd);
    if (!OBOS_HandleCloneCallbacks[type])
        return OBOS_STATUS_INVALID_OPERATION;

    handle_desc* new_desc = nullptr;
    handle new = OBOS_HandleAllocate(current_table, type, &new_desc);
    status = memcpy_k_to_usr(unew, &new, sizeof(new));
    if (obos_is_error(status))
    {
        OBOS_HandleFree(current_table, new_desc);
        return status;
    }

    void(*cb)(handle_desc *hnd, handle_desc *new) = OBOS_HandleCloneCallbacks[type];
    cb(desc, new_desc);

    return OBOS_STATUS_SUCCESS;
}
obos_status Sys_HandleClose(handle hnd)
{
    handle_table* const current_table = &CoreS_GetCPULocalPtr()->currentThread->proc->handles;

    // Get the handle descriptor.
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* desc = OBOS_HandleLookup(current_table, hnd, 0, true, &status);
    if (obos_is_error(status))
        return status;

    // Free the handle's underlying object as well as the handle itself.
    handle_type type = HANDLE_TYPE(hnd);
    void(*cb)(handle_desc *hnd) = OBOS_HandleCloseCallbacks[type];
    if (cb)
        cb(desc);
    OBOS_HandleFree(current_table, desc);

    return OBOS_STATUS_SUCCESS;
}
