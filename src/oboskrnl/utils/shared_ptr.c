/*
 * oboskrnl/utils/shared_ptr.c
 *
 * Copyright (c) 2024 Omar Berrow
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
    if (ptr->onDeref)
        ptr->onDeref(ptr);
    if (!(--ptr->refs))
        ptr->free(ptr->freeUdata, ptr);
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
