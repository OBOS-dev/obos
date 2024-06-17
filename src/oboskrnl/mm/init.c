/*
 * oboskrnl/mm/init.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include "scheduler/schedule.h"
#include <int.h>
#include <error.h>
#include <klog.h>

#include <mm/context.h>

#include <scheduler/thread.h>
#include <scheduler/process.h>

context* Mm_KernelContext;

// Quote of the VMM:
// When I wrote this, only God and I understood what I was doing.
// Now, only God knows.

void Mm_InitializeKernelContext()
{
    OBOS_ASSERT(!Mm_KernelContext);
    obos_status status = OBOS_STATUS_SUCCESS;
    Mm_KernelContext = MmH_AllocateContext(&status);
    if (obos_likely_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not allocate a vmm context object for the kernel. Status: %d\n", status);
    process* current = Core_GetCurrentThread()->proc;
    status = MmH_InitializeContext(Mm_KernelContext, current);
    if (obos_likely_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize the vmm context object for the kernel. Status: %d\n", status);
}