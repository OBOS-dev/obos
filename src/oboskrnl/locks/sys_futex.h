/*
 * oboskrnl/locks/sys_futex.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>

#include <locks/wait.h>

#include <utils/shared_ptr.h>
#include <utils/tree.h>

// NOTE: Because Linux, futex must be aligned to 4 bytes.

obos_status Sys_FutexWait(uint32_t *futex, uint32_t cmp_with, uint64_t timeout);
obos_status Sys_FutexWake(uint32_t *futex, uint32_t nWaiters);

typedef RB_HEAD(futex_tree, futex) futex_tree;
RB_PROTOTYPE(futex_tree, futex, node, cmp_futex);
typedef struct futex
{
    struct waitable_header wait_hdr;
    size_t refs;
    uint32_t *obj;
    struct context* ctx;
    RB_ENTRY(futex) node;
} futex_object;

inline static int cmp_futex(futex_object* lhs, futex_object* rhs)
{
    if (lhs->ctx < rhs->ctx)
        return -1;
    if (lhs->ctx > rhs->ctx)
        return -1;
    if (lhs->obj < rhs->obj)
        return -1;
    if (lhs->obj > rhs->obj)
        return 1;
    return 0;
}
