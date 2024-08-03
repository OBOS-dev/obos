/*
 * oboskrnl/irq/dpc.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <utils/list.h>

#include <irq/dpc.h>
#include <scheduler/cpu_local.h>
#include <scheduler/thread.h>

#include <allocators/base.h>

#include <locks/spinlock.h>

LIST_GENERATE(dpc_queue, dpc, node);

dpc* CoreH_AllocateDPC(obos_status* status)
{
    if (!OBOS_NonPagedPoolAllocator)
    {
        if (status)
            *status = OBOS_STATUS_INVALID_INIT_PHASE;
        return nullptr;
    }
    return OBOS_NonPagedPoolAllocator->Allocate(OBOS_NonPagedPoolAllocator, sizeof(dpc), status);
}
obos_status CoreH_InitializeDPC(dpc* dpc, void(*handler)(struct dpc* obj, void* userdata), thread_affinity affinity)
{
    if (!dpc || !handler)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (dpc->cpu && LIST_IS_NODE_UNLINKED(dpc_queue, &dpc->cpu->dpcs, dpc))
        return OBOS_STATUS_DPC_ALREADY_ENQUEUED;
    affinity &= Core_DefaultThreadAffinity;
    if (!affinity)
        affinity = Core_DefaultThreadAffinity;
    dpc->handler = handler;
    cpu_local* target = CoreS_GetCPULocalPtr();
    // for (size_t i = 0; i < Core_CpuCount; i++)
    //     if ((!target || Core_CpuInfo[i].dpcs.nNodes < target->dpcs.nNodes))
    //         target = (affinity & CoreH_CPUIdToAffinity(Core_CpuInfo[i].id)) ? &Core_CpuInfo[i] : nullptr;
    // If this fails, something stupid has happened.
    OBOS_ASSERT(target);
    LIST_APPEND(dpc_queue, &target->dpcs, dpc);
    dpc->cpu = target;
    return OBOS_STATUS_SUCCESS;
}
obos_status CoreH_FreeDPC(dpc* dpc)
{
    if (!dpc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!dpc->handler || !dpc->cpu)
        return OBOS_STATUS_UNINITIALIZED;
    if (!LIST_IS_NODE_UNLINKED(dpc_queue, &dpc->cpu->dpcs, dpc))
        LIST_REMOVE(dpc_queue, &dpc->cpu->dpcs, dpc);
    return OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, dpc, sizeof(*dpc));
}