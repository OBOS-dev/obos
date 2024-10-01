/*
 * oboskrnl/mm/context.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <mm/page.h>

#include <locks/spinlock.h>

#include <irq/dpc.h>

#ifdef __x86_64__
typedef uintptr_t page_table;
#elif __m68k__
// TODO:
typedef uintptr_t page_table;
#else
#   error Unknown architecture
#endif
/// <summary>
/// Populates a page structure with protection info about a page in a page table.</para>
/// Note: If the page is unmapped, the physical address should still be populated.
/// </summary>
/// <param name="pt">The page table.</param>
/// <param name="addr">The base address the page to query.</param>
/// <param name="info">[out] The page struct to put the info into. Can be nullptr.</param>
/// <param name="phys">[out] The physical address of the addr. Can be nullptr.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status MmS_QueryPageInfo(page_table pt, uintptr_t addr, page_info* info, uintptr_t* phys);
/// <summary>
/// Gets the current page table.
/// <para/>NOTE: This always returns the kernel page table.
/// </summary>
/// <returns>The current page table.</returns>
OBOS_EXPORT page_table MmS_GetCurrentPageTable();
/// <summary>
/// Updates the page mapping at page->addr to the protection in page->prot.
/// </summary>
/// <param name="pt">The page table.</param>
/// <param name="page">The page. Cannot be nullptr.</param>
/// <param name="phys">The physical page. Ignored if !page->prot.present.</param>
/// <param name="free_pte">Whether the PTEs can be freed. This parameter needs to be respected on unmap, otherwise nasty bugs could happen.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status MmS_SetPageMapping(page_table pt, const page_info* page, uintptr_t phys, bool free_pte);

typedef struct working_set
{
    struct {
        working_set_node *head, *tail;
        size_t nNodes;
    } pages;
    size_t capacity;
    size_t size;
} working_set;
#define APPEND_WORKINGSET_PAGE_NODE(list, node) do {\
	(node)->next = nullptr;\
	(node)->prev = nullptr;\
	if ((list).tail)\
		(list).tail->next = (node);\
	if (!(list).head)\
		(list).head = (node);\
	(node)->prev = ((list).tail);\
	(list).tail = (node);\
	(list).nNodes++;\
} while(0)
#define REMOVE_WORKINGSET_PAGE_NODE(list, node) do {\
	if ((list).tail == (node))\
		(list).tail = (node)->prev;\
	if ((list).head == (node))\
		(list).head = (node)->next;\
	if ((node)->prev)\
		(node)->prev->next = (node)->next;\
	if ((node)->next)\
		(node)->next->prev = (node)->prev;\
	(list).nNodes--;\
    (node)->next = nullptr;\
    (node)->prev = nullptr;\
} while(0)

typedef struct memstat
{
    // The size of all allocated (committed) memory.
    size_t committedMemory;
    // The size of all memory within this context which has been paged out.
    size_t paged;
    // The size of all pageable memory (memory that can be paged out).
    size_t pageable;
    // The size of all non-pageable memory (memory that cannot be paged out).
    size_t nonPaged;
    // The size of all uncommitted (reserved) memory. (memory allocated with VMA_FLAGS_RESERVE that has not yet been committed).
    size_t reserved;
    // The amount of total page faults on this context.
    size_t pageFaultCount;
    // The amount of soft page faults on this context.
    size_t softPageFaultCount;
    // The amount of hard page faults on this context.
    size_t hardPageFaultCount;
    // The amount of page faults on this context since the last sampling interval.
    size_t pageFaultCountSinceSample;
    // The amount of soft page faults on this context since the last sampling interval.
    size_t softPageFaultCountSinceSample;
    // The amount of hard page faults on this context since the last sampling interval.
    size_t hardPageFaultCountSinceSample;
} memstat;
typedef struct context
{
    struct process* owner;
    page_tree pages;
    working_set workingSet;
    // The pages referenced since the last run of the page replacement algorithm.
    struct {
        working_set_node *head, *tail;
        size_t nNodes;
    } referenced;
    spinlock lock;
    page_table pt;
    dpc file_mapping_dpc;
    memstat stat;
} context;
extern OBOS_EXPORT context Mm_KernelContext;
extern char MmS_MMPageableRangeStart[];
extern char MmS_MMPageableRangeEnd[];
bool MmH_IsAddressUnPageable(uintptr_t addr);