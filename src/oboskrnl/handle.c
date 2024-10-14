/*
 * oboskrnl/handle.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <handle.h>

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
handle_desc* OBOS_HandleLookup(handle_table* table, handle hnd, handle_type type)
{
    OBOS_ASSERT(table);
    // First, validate the handle type.
    if ((hnd >> 24) != type)
        return nullptr;
    hnd &= 0xffffff;
    if (hnd >= table->size)
        return nullptr;
    if (in_range(table->arr, table->arr+table->size, table->arr[hnd].un.next))
        return nullptr; // use-after-free
    if (!table->arr[hnd].un.next)
        return nullptr; // use-after-free
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
        hnd = table->last_handle++;
    *desc = &table->arr[hnd];
    hnd |= (type << 24);
    Core_MutexRelease(&table->lock);
    return hnd;
}
void OBOS_HandleFree(handle_table* table, handle hnd)
{
    OBOS_ASSERT(table);
    hnd &= 0xffffff;
    if (hnd >= table->size)
        return;
    Core_MutexAcquire(&table->lock);
    handle_desc* curr = &table->arr[hnd];
    curr->un.next = table->head;
    table->head = curr;
    Core_MutexRelease(&table->lock);
    // any use of this handle past here is a use-after-free
}
