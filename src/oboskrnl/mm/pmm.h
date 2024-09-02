/*
	oboskrnl/mm/pmm.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <scheduler/thread.h>

extern size_t Mm_TotalPhysicalPages;
extern size_t Mm_TotalPhysicalPagesUsed;
extern size_t Mm_UsablePhysicalPages;
extern uintptr_t Mm_PhysicalMemoryBoundaries;

/// <summary>
/// Initializes the PMM.
/// </summary>
/// <returns>The function status.</returns>
obos_status Mm_InitializePMM();

/// <summary>
/// Allocates physical pages.
/// </summary>
/// <param name="nPages">The amount of physical pages to allocate.</param>
/// <param name="alignmentPages">The alignment of the address returned, in pages.</param>
/// <param name="status">[optional] A pointer to a variable that will store the function's status. Can be nullptr.</param>
/// <returns>The physical pages, or zero on failure.</returns>
OBOS_EXPORT uintptr_t Mm_AllocatePhysicalPages(size_t nPages, size_t alignmentPages, obos_status* status);
/// <summary>
/// Allocates physical pages ranging from 0x0-0xffffffff.</br>
/// This is the same as Mm_AllocatePhysicalPages on 32-bit architectures.
/// </summary>
/// <param name="nPages">The amount of physical pages to allocate.</param>
/// <param name="alignmentPages">The alignment of the address returned, in pages.</param>
/// <param name="status">[optional] A pointer to a variable that will store the function's status. Can be nullptr.</param>
/// <returns>The physical pages, or zero on failure.</returns>
OBOS_EXPORT uintptr_t Mm_AllocatePhysicalPages32(size_t nPages, size_t alignmentPages, obos_status* status);
/// <summary>
/// Frees physical pages.
/// </summary>
/// <param name="addr">The address of the page.</param>
/// <param name="nPages">The amount of pages to free.</param>
OBOS_EXPORT obos_status Mm_FreePhysicalPages(uintptr_t addr, size_t nPages);

// This returns a virtual address given a physical address.
// For example, on x86-64, this can offset the physical address by the hhdm.
OBOS_EXPORT void* MmS_MapVirtFromPhys(uintptr_t addr);
OBOS_EXPORT uintptr_t MmS_UnmapVirtFromPhys(void* virt);

#ifdef __x86_64__
#	include <arch/x86_64/pmm.h>
#elif defined(__m68k__)
#	include <arch/m68k/pmm.h>
#else
#	error Unknown architecture!
#endif

obos_pmem_map_entry* MmS_GetFirstPMemMapEntry(uintptr_t* index);
// returns nullptr at the end of the list.
obos_pmem_map_entry* MmS_GetNextPMemMapEntry(obos_pmem_map_entry* current, uintptr_t* index);