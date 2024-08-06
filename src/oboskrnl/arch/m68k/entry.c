/*
 * oboskrnl/arch/m68k/entry.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>

#include <allocators/base.h>

#include <scheduler/process.h>

#include <irq/irql.h>
#include <irq/timer.h>

allocator_info* OBOS_KernelAllocator;
process *OBOS_KernelProcess;
timer_frequency CoreS_TimerFrequency;

// DO NOT DELETE

OBOS_PAGEABLE_VARIABLE int Arch_PageableDataVar;
OBOS_PAGEABLE_RO_VARIABLE const int Arch_PageableRodataVar;

void Arch_KernelEntryBootstrap()
{
    // We can't really do anything yet.
    // Hang.
    while(1);
}