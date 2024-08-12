/*
 * oboskrnl/mm/context.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <text.h>

#include <mm/context.h>
#include <mm/page.h>

#include <scheduler/cpu_local.h>

#include <utils/tree.h>

#include <scheduler/process.h>
#include <scheduler/schedule.h>

#include <irq/irq.h>

#define round_up(addr) (uintptr_t)((uintptr_t)(addr) + (OBOS_PAGE_SIZE - ((uintptr_t)(addr) % OBOS_PAGE_SIZE)))
#define round_down(addr) (uintptr_t)((uintptr_t)(addr) - ((uintptr_t)(addr) % OBOS_PAGE_SIZE))
bool MmH_IsAddressUnPageable(uintptr_t addr)
{
	if (addr >= round_down(Core_CpuInfo) && addr < round_up((uintptr_t)(Core_CpuInfo + Core_CpuCount)))
		return true;
	if (addr >= round_down(OBOS_TextRendererState.fb.base) && addr < round_up((uintptr_t)OBOS_TextRendererState.fb.base + OBOS_TextRendererState.fb.height*OBOS_TextRendererState.fb.pitch))
		return true;
	if (addr >= round_down(OBOS_KernelProcess->threads.head->data) &&
		addr < round_up(OBOS_KernelProcess->threads.head->data + 1))
		return true;
	if (addr >= round_down(OBOS_KernelProcess->threads.head->data->snode) &&
		addr < round_up(OBOS_KernelProcess->threads.head->data->snode + 1))
		return true;
	if (addr >= round_down(Core_SchedulerIRQ) &&
		addr < round_up(Core_SchedulerIRQ + 1))
		return true;
	if (addr >= round_down(OBOS_KernelProcess) &&
		addr < round_up(OBOS_KernelProcess + 1))
		return true;
	for (size_t i = 0; i < Core_CpuCount; i++)
	{
		cpu_local* cpu = &Core_CpuInfo[i];
#ifdef __x86_64__
        if (addr >= round_down(cpu->idleThread) &&
			addr < round_up(cpu->idleThread + 1))
			return true;
        if (addr >= round_down(cpu->idleThread->snode) &&
			addr < round_up(cpu->idleThread->snode + 1))
			return true;
		if (addr >= round_down(cpu->idleThread->context.stackBase) &&
			addr < round_up((uintptr_t)cpu->idleThread->context.stackBase + cpu->idleThread->context.stackSize))
			return true;
        if (addr >= round_down(cpu->arch_specific.ist_stack) &&
			addr < round_up((uintptr_t)cpu->arch_specific.ist_stack + 0x20000))
			return true;
#elif defined(__m68k__)
		if (addr >= round_down(cpu->idleThread) &&
			addr < round_up(cpu->idleThread + 1))
			return true;
        if (addr >= round_down(cpu->idleThread->snode) &&
			addr < round_up(cpu->idleThread->snode + 1))
			return true;
		if (addr >= round_down(cpu->idleThread->context.stackBase) &&
			addr < round_up((uintptr_t)cpu->idleThread->context.stackBase + cpu->idleThread->context.stackSize))
			return true;
#else
#	error Unknown architecture.
#endif
	}
    if (!(addr >= round_down(&MmS_MMPageableRangeStart) && addr < round_up(&MmS_MMPageableRangeEnd)))
		return true;
	return false;
}
RB_GENERATE(page_tree, page, rb_node, pg_cmp_pages);