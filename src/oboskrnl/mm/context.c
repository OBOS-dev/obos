/*
 * oboskrnl/mm/context.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "scheduler/cpu_local.h"
#include "text.h"
#include <int.h>
#include <error.h>
#include <klog.h>

#include <stdint.h>
#include <utils/tree.h>

#include <mm/context.h>
#include <mm/pg_node.h>

#include <scheduler/process.h>

// Quote of the VMM:
// When I wrote this, only God and I understood what I was doing.
// Now, only God knows.

allocator_info* Mm_VMMAllocator;
OBOS_EXCLUDE_FUNC_FROM_MM int cmp_page_node(const page_node* right, const page_node* left)
{
	if (right->addr == left->addr)
		return 0;
	return right->addr < left->addr ? -1 : 1;
}
RB_GENERATE_INTERNAL(page_node_tree, page_node, rb_tree_node, cmp_page_node, OBOS_EXCLUDE_FUNC_FROM_MM);
context* MmH_AllocateContext(obos_status* status)
{
	return Mm_VMMAllocator->ZeroAllocate(Mm_VMMAllocator, 1, sizeof(context), status);
}
obos_status MmH_InitializeContext(context* ctx, struct process* owner)
{
	if (!ctx || !owner)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!owner->pid)
		return OBOS_STATUS_INVALID_ARGUMENT;
	ctx->owner = owner;
	owner->ctx = ctx;
	ctx->workingSet.size = 4*1024*1024 /* 4 MiB */;
	return OBOS_STATUS_SUCCESS;
}
page_node* MmH_AllocatePageNode(obos_status* status)
{
	return Mm_VMMAllocator->ZeroAllocate(Mm_VMMAllocator, 1, sizeof(page_node), status); 
}
OBOS_EXCLUDE_FUNC_FROM_MM void MmH_RegisterUse(page_node* pg)
{
	if (!pg)
		return;
	pg->uses |= 1;
	pg->uses >>= 1;
}
OBOS_EXCLUDE_FUNC_FROM_MM uint8_t MmH_LogicalSumOfUses(uint8_t uses)
{
	// We can return the popcount of the uses to basically 'or' all the bits.
	return __builtin_popcount(uses) >= 1;
}
#define round_up(addr) (uintptr_t)((uintptr_t)(addr) + (OBOS_PAGE_SIZE - ((uintptr_t)(addr) % OBOS_PAGE_SIZE)))
#define round_down(addr) (uintptr_t)((uintptr_t)(addr) - ((uintptr_t)(addr) % OBOS_PAGE_SIZE))
bool MmH_AddressExcluded(uintptr_t addr)
{
	// If the page is:
	// In the exclusion range,
	// a cpu info struct, or
	// A cpu temporary stack range,
	// ignore it.
	if (addr >= round_down(&MmS_MMExclusionRangeStart) && addr < round_up(&MmS_MMExclusionRangeEnd))
		return true;
	if (addr >= round_down(Core_CpuTempStackBase) && addr < round_up(((uintptr_t)Core_CpuTempStackBase) + Core_CpuTempStackSize*Core_CpuCount))
		return true;
	if (addr >= round_down(Core_CpuInfo) && addr < round_up((uintptr_t)(Core_CpuInfo + Core_CpuCount)))
		return true;
	if (addr >= round_down(OBOS_TextRendererState.fb.base) && addr < round_up((uintptr_t)OBOS_TextRendererState.fb.base + OBOS_TextRendererState.fb.height*OBOS_TextRendererState.fb.pitch))
		return true;
	// Check CPU idle thread stacks.
	for (size_t i = 0; i < Core_CpuCount; i++)
	{
		cpu_local* cpu = &Core_CpuInfo[i];
#ifdef __x86_64
		if (addr >= round_down(cpu->idleThread->context.stackBase) &&
			addr < round_up((uintptr_t)cpu->idleThread->context.stackBase + cpu->idleThread->context.stackSize))
			return true;
#else
#	error Unknown architecture.
#endif
	}
	return false;
}