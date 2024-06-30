/*
 * oboskrnl/mm/context.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <stdint.h>
#include <utils/tree.h>

#include <mm/pg_node.h>

#include <allocators/base.h>

#include <locks/spinlock.h>

// Each arch shall typedef an abstract (to the non arch-specific code) type representing the top-level page table.
// typedef abstract pt_context;
// Example: On x86-64, this would be defined as uintptr_t to represent the PML4 table (in cr3).
// It must also define macros which act as function attributes that exclude a function or variable from the MM. (puts it in the MmS_MMExclusionRange)
// #define OBOS_EXCLUDE_FUNC_FROM_MM __attribute__((section(".no.mm.text")))
// #define OBOS_EXCLUDE_VAR_FROM_MM __attribute__((section(".no.mm.data")))
// #define OBOS_EXCLUDE_CONST_VAR_FROM_MM __attribute__((section(".no.mm.rodata")))
#ifdef __x86_64__
typedef uintptr_t pt_context;
#define OBOS_EXCLUDE_FUNC_FROM_MM __attribute__((section(".no.mm.text")))
#define OBOS_EXCLUDE_VAR_FROM_MM __attribute__((section(".no.mm.data")))
#define OBOS_EXCLUDE_CONST_VAR_FROM_MM __attribute__((section(".no.mm.rodata")))
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
/// <param name="virt">The virtual address to check. Must be page aligned.</param>
/// <param name="info">A pointer to the page info struct.</param>
/// <returns>The status of the function.</returns>
obos_status MmS_PTContextQueryPageInfo(const pt_context* ctx, uintptr_t virt, pt_context_page_info* info);
/// <summary>
/// Gets the current pt_context.
/// </summary>
/// <returns>The current pt_context.</returns>
pt_context MmS_PTContextGetCurrent();
/// <summary>
/// Gets a direct map for a physical address.
/// </summary>
/// <param name="phys">The physical address.</param>
/// <returns>The address of the map.</returns>
void* MmS_GetDM(uintptr_t phys);

RB_HEAD(page_node_tree, page_node);
typedef struct page_node_list
{
	page_node *head, *tail;
	size_t nNodes;
} page_node_list;
#define APPEND_PAGE_NODE(list, node) do {\
	(node)->linked_list_node.next = nullptr;\
	(node)->linked_list_node.prev = nullptr;\
	if ((list).tail)\
		(list).tail->linked_list_node.next = (node);\
	if (!(list).head)\
		(list).head = (node);\
	(node)->linked_list_node.prev = (list.tail);\
	(list).tail = (node);\
	(list).nNodes++;\
} while(0)
#define REMOVE_PAGE_NODE(list, node) do {\
	if ((list).tail == (node))\
		(list).tail = (node)->linked_list_node.prev;\
	if ((list).head == (node))\
		(list).head = (node)->linked_list_node.next;\
	if ((node)->linked_list_node.prev)\
		(node)->linked_list_node.prev->linked_list_node.next = (node)->linked_list_node.next;\
	if ((node)->linked_list_node.next)\
		(node)->linked_list_node.next->linked_list_node.prev = (node)->linked_list_node.prev;\
	(list).nNodes--;\
} while(0)
typedef struct context
{
	struct process* owner;
	struct page_node_tree pageNodeTree;
	struct
	{
		page_node_list list;
		size_t size;
	} workingSet;
	page_node_list pagesReferenced;
	pt_context pt_ctx;
	spinlock lock;
} context;
extern int cmp_page_node(const page_node* right, const page_node* left);
RB_PROTOTYPE(page_node_tree, page_node, rb_tree_node, cmp_page_node);
extern allocator_info* Mm_VMMAllocator;
context* MmH_AllocateContext(obos_status* status);
obos_status MmH_InitializeContext(context* ctx, struct process* owner);
extern context* Mm_KernelContext;
// Never to be dereferenced.
// The address of these are their values.

extern char MmS_MMExclusionRangeStart[];
extern char MmS_MMExclusionRangeEnd[];

bool MmH_AddressExcluded(uintptr_t addr);