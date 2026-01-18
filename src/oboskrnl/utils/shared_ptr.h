/*
 * oboskrnl/utils/shared_ptr.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>

typedef struct shared_ptr
{
    _Atomic(size_t) refs;
    void* obj;
    size_t szObj;
    // Can be nullptr
    // Frees obj.
    void(*free)(void* udata, struct shared_ptr*);
    void* freeUdata;
    // Can be nullptr
    // Called after the ref count is decreased, but before
    // the object is freed.
    void(*onDeref)(struct shared_ptr*);
    // Can be nullptr
    // Called after the ref count is increased.
    void(*onRef)(struct shared_ptr*);
} shared_ptr;

OBOS_EXPORT shared_ptr* OBOS_SharedPtrConstructSz(shared_ptr* ptr, void* obj, size_t sz);
#define OBOS_SharedPtrConstruct(ptr, obj_ptr) ({ typeof(obj_ptr) _a = (obj_ptr); OBOS_SharedPtrConstructSz((ptr), _a, sizeof(*_a)); })
OBOS_EXPORT void OBOS_SharedPtrRef(shared_ptr* ptr);
#define OBOS_SharedPtrCopy(ptr) ({OBOS_SharedPtrRef((ptr)); ptr; })
OBOS_EXPORT void OBOS_SharedPtrUnref(shared_ptr* ptr);
// udata is the struct allocator_info* used to allocate the object.
// if udata is nullptr, OBOS_KernelAllocator is assumed
OBOS_EXPORT void OBOS_SharedPtrDefaultFree(void* udata, shared_ptr* ptr);
OBOS_EXPORT void OBOS_SharedPtrAssertRefs(shared_ptr* ptr);
#define OBOS_SharedPtrGet(type, ptr_) ({ typeof(ptr_) _a = (ptr_); OBOS_SharedPtrAssertRefs(_a); (type*)(_a->obj); })
