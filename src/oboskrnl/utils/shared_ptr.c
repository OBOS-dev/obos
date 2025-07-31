/*
 * oboskrnl/utils/shared_ptr.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <klog.h>

#include <allocators/base.h>

#include <utils/shared_ptr.h>

shared_ptr* OBOS_SharedPtrConstructSz(shared_ptr* ptr, void* obj, size_t sz)
{
    ptr->obj = obj;
    ptr->szObj = sz;
    ptr->refs = 0;
    ptr->free = nullptr;
    ptr->freeUdata = nullptr;
    ptr->onDeref = ptr->onRef = nullptr;
    return ptr;
}

void OBOS_SharedPtrRef(shared_ptr* ptr)
{
    OBOS_ASSERT(ptr);
    if (!ptr)
        return;
    // printf("%p refed shared ptr %p (%d->%d)\n",  __builtin_return_address(0), ptr, ptr->refs, ptr->refs+1);
    ptr->refs++;
    if (ptr->onRef)
        ptr->onRef(ptr);
}

void OBOS_SharedPtrUnref(shared_ptr* ptr)
{
    OBOS_ASSERT(ptr);
    if (!ptr)
        return;
    OBOS_ASSERT(ptr->refs);
    --ptr->refs;
    // printf("%p unrefed shared ptr %p (%d->%d)\n",  __builtin_return_address(0), ptr, ptr->refs+1, ptr->refs);
    if (!ptr->refs && ptr->free)
        ptr->free(ptr->freeUdata, ptr);
    if (ptr->onDeref)
        ptr->onDeref(ptr);
}

void OBOS_SharedPtrAssertRefs(shared_ptr* ptr)
{
    OBOS_ASSERT(ptr->refs);
}

void OBOS_SharedPtrDefaultFree(void* udata, shared_ptr* ptr)
{
    struct allocator_info* alloc = udata ? udata : OBOS_KernelAllocator;
    alloc->Free(alloc, ptr->obj, ptr->szObj);
}
