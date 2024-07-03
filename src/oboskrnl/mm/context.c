/*
 * oboskrnl/mm/context.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <text.h>

#include <mm/context.h>

#include <scheduler/cpu_local.h>

#define round_up(addr) (uintptr_t)((uintptr_t)(addr) + (OBOS_PAGE_SIZE - ((uintptr_t)(addr) % OBOS_PAGE_SIZE)))
#define round_down(addr) (uintptr_t)((uintptr_t)(addr) - ((uintptr_t)(addr) % OBOS_PAGE_SIZE))
bool MmH_IsAddressPageable(uintptr_t addr)
{
    if (addr >= round_down(&MmS_MMExclusionRangeStart) && addr < round_up(&MmS_MMExclusionRangeEnd))
		return true;
	if (addr >= round_down(Core_CpuInfo) && addr < round_up((uintptr_t)(Core_CpuInfo + Core_CpuCount)))
		return true;
	if (addr >= round_down(OBOS_TextRendererState.fb.base) && addr < round_up((uintptr_t)OBOS_TextRendererState.fb.base + OBOS_TextRendererState.fb.height*OBOS_TextRendererState.fb.pitch))
		return true;
	// Check CPU idle thread and temporary stacks.
	for (size_t i = 0; i < Core_CpuCount; i++)
	{
		cpu_local* cpu = &Core_CpuInfo[i];
#ifdef __x86_64
		if (addr >= round_down(cpu->idleThread->context.stackBase) &&
			addr < round_up((uintptr_t)cpu->idleThread->context.stackBase + cpu->idleThread->context.stackSize))
			return true;
        if (addr >= round_down(cpu->arch_specific.ist_stack) &&
			addr < round_up((uintptr_t)cpu->arch_specific.ist_stack + 0x20000))
			return true;
#else
#	error Unknown architecture.
#endif
	}
	return false;
}