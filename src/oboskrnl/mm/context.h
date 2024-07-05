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

#ifdef __x86_64__
typedef uintptr_t page_table;
#else
#   error Unknown architecture
#endif
/// <summary>
/// Populates a page structure with protection info about a page in a page table.
/// </summary>
/// <param name="pt">The page table.</param>
/// <param name="addr">The base address the page to query.</param>
/// <param name="info">[out] The page struct to put the info into. Mustn't be nullptr.</param>
/// <returns>The status of the function.</returns>
obos_status MmS_QueryPageInfo(page_table pt, uintptr_t addr, page* info);
/// <summary>
/// Gets the current page table.
/// </summary>
/// <returns>The current page table.</returns>
page_table MmS_GetCurrentPageTable();
/// <summary>
/// Updates the page mapping at page->addr to the protection in page->prot.
/// </summary>
/// <param name="pt">The page table.</param>
/// <param name="page">The page. Cannot be nullptr.</param>
/// <param name="phys">The physical page. Ignored if !page->prot.present.</param>
/// <returns>The status of the function.</returns>
obos_status MmS_SetPageMapping(page_table pt, const page* page, uintptr_t phys);

typedef struct working_set
{
    page_list pages;
    size_t size;
} working_set;
typedef struct context
{
    struct process* owner;
    page_tree pages;
    working_set workingSet;
    // The pages referenced since the last run of the page replacement algorithm.
    page_list referenced;
    spinlock lock;
    page_table pt;
} context;
extern context Mm_KernelContext;
extern char MmS_MMExclusionRangeStart[];
extern char MmS_MMExclusionRangeEnd[];
bool MmH_IsAddressUnPageable(uintptr_t addr);