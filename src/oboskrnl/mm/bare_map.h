/*
 * oboskrnl/mm/bare_map.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

/// <summary>
/// Maps a page as Read, Write, and execution disabled.<br></br>
/// This should not fail if the address at 'at' is already allocated.
/// </summary>
/// <param name="at">The address of the page to map.</param>
/// <param name="phys">The physical address that should correspond to said page.</param>
/// <returns>The status of the function.</returns>
obos_status OBOSS_MapPage_RW_XD(void* at, uintptr_t phys);
/// <summary>
/// Unmaps a page.
/// </summary>
/// <param name="at">The address of the page to unmap.</param>
/// <returns>The status of the function.</returns>
obos_status OBOSS_UnmapPage(void* at);

/// <summary>
/// Allocates physical pages.
/// </summary>
/// <param name="nPages">The amount of pages to allocate.</param>
/// <param name="alignment">The alignment (in pages) of the address returned. Must be a power of two.</param>
/// <param name="status">[out,optional] The function's status. Can be nullptr.</param>
/// <returns>The address of the allocated region.</returns>
uintptr_t OBOSS_AllocatePhysicalPages(size_t nPages, size_t alignment, obos_status* status);
/// <summary>
/// Frees physical pages.
/// </summary>
/// <param name="base">The base of said pages.</param>
/// <param name="nPages">The amount of pages to free.</param>
/// <returns>The status of the function.</returns>
obos_status OBOSS_FreePhysicalPages(uintptr_t base, size_t nPages);

/// <summary>
/// Allocates pages as RW XD.<br></br>
/// Only to be used in kernel-mode.
/// </summary>
/// <param name="sz">The size of the region to be returned.</param>
/// <param name="status">[out,optional] The function's status. Can be nullptr.</param>
/// <returns>The region allocated, or nullptr on failure (see value set in status parameter). The region does not have to be page aligned.</returns>
void* OBOS_BasicMMAllocatePages(size_t sz, obos_status* status);
/// <summary>
/// Frees pages previously allocated by OBOS_AllocatePages<br></br>
/// Like OBOS_AllocatePages, this is only to be used in kernel-mode.
/// </summary>
/// <param name="base">The base of the allocated region. Must be the same address returned by OBOS_AllocatePages, or the function will fail.</param>
/// <param name="sz">The size of the allocated region.</param>
/// <returns>The status of the function.</returns>
obos_status OBOS_BasicMMFreePages(void *base, size_t sz);

/// <summary>
/// Gets the size of a region. This always returns the same thing.
/// </summary>
/// <returns>The size of a region.</returns>
size_t OBOSH_BasicMMGetRegionSize();
/// <summary>
/// Adds a region to the region nodes for the basic mm for OBOS.
/// </summary>
/// <param name="nodeBase">The address of the node. The size of this should be greater than or equal to OBOSH_BasicMMGetRegionSize's return value.</param>
/// <param name="base">The base of the region.</param>
/// <param name="sz">The size of the region.</param>
void OBOSH_BasicMMAddRegion(void* nodeBase, void* base, size_t sz);