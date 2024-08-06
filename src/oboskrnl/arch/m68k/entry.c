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

void Arch_KernelEntryBootstrap()
{
    // We can't really do anything yet.
    // Hang.
    while(1);
}