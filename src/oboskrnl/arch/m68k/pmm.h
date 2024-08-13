/*
	oboskrnl/arch/x86_64/pmm.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <stdint.h>

extern size_t Arch_TotalPhysicalPages;
extern size_t Arch_TotalPhysicalPagesUsed;
extern size_t Arch_UsablePhysicalPages;
extern uintptr_t Arch_PhysicalMemoryBoundaries;

/// <summary>
/// Initializes the PMM.
/// </summary>
/// <returns>The function status.</returns>
obos_status Arch_InitializePMM();

/// <summary>
/// Allocates physical pages.
/// </summary>
/// <param name="nPages">The amount of physical pages to allocate.</param>
/// <param name="alignmentPages">The alignment of the address returned, in pages.</param>
/// <param name="status">[optional] A pointer to a variable that will store the function's status. Can be nullptr.</param>
/// <returns>The physical pages, or zero on failure.</returns>
uintptr_t Arch_AllocatePhysicalPages(size_t nPages, size_t alignmentPages, obos_status* status);
/// <summary>
/// Frees physical pages.
/// </summary>
/// <param name="addr">The address of the page.</param>
/// <param name="nPages">The amount of pages to free.</param>
void Arch_FreePhysicalPages(uintptr_t addr, size_t nPages);
void* Arch_MapToHHDM(uintptr_t phys);
uintptr_t Arch_UnmapFromHHDM(void* virt);