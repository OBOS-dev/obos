/*
 * oboskrnl/mm/bare_map.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

/// <summary>
/// Maps a page as Read, Write, and execution disabled. This page should not accessible from user-mode.<para/>
/// This should not fail if the address at 'at' is already allocated.
/// </summary>
/// <param name="at">The address of the page to map.</param>
/// <param name="phys">The physical address that should correspond to said page.</param>
/// <returns>The status of the function.</returns>
OBOS_WEAK obos_status OBOSS_MapPage_RW_XD(void* at, uintptr_t phys);
/// <summary>
/// Unmaps a page.
/// </summary>
/// <param name="at">The address of the page to unmap.</param>
/// <returns>The status of the function.</returns>
OBOS_WEAK obos_status OBOSS_UnmapPage(void* at);
/// <summary>
/// Queries the physical address of an address.
/// </summary>
/// <param name="at">The address to query.</param>
/// <param name="oPhys">[out] The physical address.</param>
/// <returns>The status of the function.</returns>
OBOS_WEAK obos_status OBOSS_GetPagePhysicalAddress(void* at, uintptr_t* oPhys);

typedef struct basicmm_region
{
	union
	{
		uint64_t integer;
		char signature[8];
	} magic;
	uintptr_t addr;
	bool mmioRange;
	size_t size;
	struct basicmm_region* next;
	struct basicmm_region* prev;
} basicmm_region;

/// <summary>
/// Allocates pages as RW XD.<para/>
/// Only to be used in kernel-mode.
/// </summary>
/// <param name="sz">The size of the region to be returned.</param>
/// <param name="status">[out,optional] The function's status. Can be nullptr.</param>
/// <returns>The region allocated, or nullptr on failure (see value set in status parameter). The region does not have to be page aligned.</returns>
void* OBOS_BasicMMAllocatePages(size_t sz, obos_status* status);
/// <summary>
/// Frees pages previously allocated by OBOS_BasicMMAllocatePages<para/>
/// Like OBOS_BasicMMAllocatePages, this is only to be used in kernel-mode.
/// </summary>
/// <param name="base">The base of the allocated region. Must be the same address returned by OBOS_AllocatePages, or the function will fail.</param>
/// <param name="sz">The size of the allocated region.</param>
/// <returns>The status of the function.</returns>
obos_status OBOS_BasicMMFreePages(void *base, size_t sz);

/// <summary>
/// Adds a region to the region nodes for the basic mm for OBOS.
/// </summary>
/// <param name="nodeBase">The address of the node.</param>
/// <param name="base">The base of the region.</param>
/// <param name="sz">The size of the region.</param>
void OBOSH_BasicMMAddRegion(basicmm_region* nodeBase, void* base, size_t sz);
/// <summary>
/// Iterates over regions in the basic mm for OBOS.
/// </summary>
/// <param name="callback">The call back. Returns true to continue iteration, otherwise false. Takes in the current region, and the passed user data.</param>
/// <param name="udata">The user data to pass.</param>
void OBOSH_BasicMMIterateRegions(bool(*callback)(basicmm_region*, void*), void* udata);