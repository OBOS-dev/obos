/*
 * oboskrnl/irq/dpc.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <utils/list.h>

#include <scheduler/thread.h>

typedef LIST_HEAD(dpc_queue, struct dpc) dpc_queue;
LIST_PROTOTYPE(dpc_queue, struct dpc, node);

typedef struct dpc
{
    LIST_NODE(dpc_queue, struct dpc) node;
    // The handler mustn't ever lower the irql below IRQL_DISPATCH
    // This is called at IRQL_DISPATCH.
    void(*handler)(struct dpc* dpc, void* userdata);
    void* userdata;
    struct cpu_local* cpu;
} dpc;

OBOS_EXPORT dpc* CoreH_AllocateDPC(obos_status* status);
OBOS_EXPORT obos_status CoreH_InitializeDPC(dpc* dpc, void(*handler)(struct dpc* obj, void* userdata), thread_affinity affinity);
OBOS_EXPORT obos_status CoreH_FreeDPC(dpc* dpc, bool dealloc);
