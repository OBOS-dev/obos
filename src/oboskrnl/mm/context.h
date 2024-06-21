/*
 * oboskrnl/mm/context.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <utils/tree.h>

#include <mm/pg_node.h>

#include <allocators/base.h>

// Each arch shall typedef an abstract (to the non arch-specific code) type representing the top-level page table.
// typedef abstract pt_context;
// Example: On x86-64, this would be defined as uintptr_t to represent the PML4 table (in cr3).
#ifdef __x86_64__
typedef uintptr_t pt_context;
#else
#error Unknown architecture.
#endif
typedef struct pt_context_page_info
{
	uintptr_t addr;
	uintptr_t phys;
	bool present : 1;
	bool huge_page : 1;
	bool dirty : 1;
	bool accessed : 1;
	prot_flags protection;
} pt_context_page_info;
/// <summary>
/// Initializes an empty (user-space) context.
/// </summary>
/// <param name="ctx">[out] A pointer to the context to initialize. Cannot be nullptr.</param>
/// <returns>The status of the function.</returns>
obos_status MmS_PTContextInitialize(pt_context* ctx);
/// <summary>
/// (Re-,un-)maps an address into a context.
/// </summary>
/// <param name="ctx">A pointer to the context to map into. Cannot be nullptr.</param>
/// <param name="virt">The virtual address to map.</param>
/// <param name="phys">The physical address to back the virtual address.</param>
/// <param name="prot">The protection flags. Ignored if present == false.</param>
/// <param name="present">Whether the page should be removed or (re-)mapped.</param>
/// <param name="isHuge">Whether the page size is that of a large page or that of a normal page. This can be ignored if the arch doesn't support huge pages.</param>
/// <returns>The status of the function.</returns>
obos_status MmS_PTContextMap(pt_context* ctx, uintptr_t virt, uintptr_t phys, prot_flags prot, bool present, bool isHuge);
/// <summary>
/// Checks if a page is huge or not.
/// </summary>
/// <param name="ctx">A pointer to the context. Cannot be nullptr.</param>
/// <param name="virt">The virtual address to check. Must be huge-page aligned.</param>
/// <param name="info">A pointer to the page info struct.</param>
/// <returns>The status of the function.</returns>
obos_status MmS_PTContextQueryPageInfo(pt_context* ctx, uintptr_t virt, pt_context_page_info* info);
/// <summary>
/// Gets the current pt_context.
/// </summary>
/// <returns>The current pt_context.</returns>
pt_context MmS_PTContextGetCurrent();

RB_HEAD(page_node_tree, page_node);
typedef struct page_node_list
{
	page_node *head, *tail;
	size_t nNodes;
} page_node_list;
typedef struct context
{
	struct process* owner;
	struct page_node_tree pageNodeTree;
	struct
	{
		page_node_list list;
		size_t size;
	} workingSet;
	pt_context pt_ctx;
} context;
extern int cmp_page_node(page_node* right, page_node* left);
RB_PROTOTYPE(page_node_tree, page_node, rb_tree_node, cmp_page_node);
extern allocator_info* Mm_VMMAllocator;
context* MmH_AllocateContext(obos_status* status);
obos_status MmH_InitializeContext(context* ctx, struct process* owner);
extern context* Mm_KernelContext;