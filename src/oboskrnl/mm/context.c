/*
 * oboskrnl/mm/context.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>

#include <utils/tree.h>

#include <mm/context.h>
#include <mm/pg_node.h>

#include <scheduler/process.h>

// Quote of the VMM:
// When I wrote this, only God and I understood what I was doing.
// Now, only God knows.

allocator_info* Mm_VMMAllocator;
int cmp_page_node(page_node* right, page_node* left)
{
	if (right->addr == left->addr)
		return 0;
	return right->addr < left->addr ? -1 : 1;
}
RB_GENERATE(page_node_tree, page_node, rb_tree_node, cmp_page_node);
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
void MmH_RegisterUse(page_node* pg)
{
	if (!pg)
		return;
	pg->uses |= 1;
	pg->uses >>= 1;
}
uint8_t MmH_LogicalSumOfUses(uint8_t uses)
{
	// We can return the popcount of the uses to basically 'or' all the bits.
	return __builtin_popcount(uses) & 1;
}