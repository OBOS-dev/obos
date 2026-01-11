/*
 * oboskrnl/mm/alloc.h
 * 
 * Copyright (c) 2024-2026 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/limits.h>

#include <mm/context.h>

#include <vfs/fd.h>

typedef enum vma_flags
{
    VMA_FLAGS_HUGE_PAGE = BIT(0),
    VMA_FLAGS_GUARD_PAGE = BIT(2),
    VMA_FLAGS_32BIT = BIT(3),
    VMA_FLAGS_HINT = BIT(4),
    VMA_FLAGS_NON_PAGED = BIT(5),
	VMA_FLAGS_PRIVATE = BIT(6), // only applies when mapping a file.
	VMA_FLAGS_PREFAULT = BIT(7), // only applies when mapping a file.
	VMA_FLAGS_32BITPHYS = BIT(9), // 32-bit physical addresses should be allocated. Best to use with VMA_FLAGS_NON_PAGED. Ignored if file != nullptr.
	VMA_FLAGS_NO_FORK = BIT(10),
    VMA_FLAGS_FRAMEBUFFER = BIT(11), // Overrides OBOS_PROTECTION_CACHE_DISABLE
    VMA_FLAGS_POSIX_COMPAT = BIT(12),
	VMA_FLAGS_KERNEL_STACK = VMA_FLAGS_NON_PAGED|VMA_FLAGS_GUARD_PAGE,
} vma_flags;

typedef enum prot_flags
{
	/// <summary>
	/// Allocates the pages as read-only.
	/// </summary>
	OBOS_PROTECTION_READ_ONLY = 0x1,
	/// <summary>
	/// Allows execution on the pages. Might not be supported on some architectures.
	/// </summary>
	OBOS_PROTECTION_EXECUTABLE = 0x2,
	/// <summary>
	/// Allows user-mode threads to read the allocated pages. Note: On some architectures, in some configurations, this might page fault in kernel-mode.
	/// </summary>
	OBOS_PROTECTION_USER_PAGE = 0x4,
	/// <summary>
	/// Disables cache on the pages. Should not be allowed for most user programs.
	/// </summary>
	OBOS_PROTECTION_CACHE_DISABLE = 0x8,
	/// <summary>
	/// For Mm_VirtualMemoryProtect. Sets the protection to the same thing it was before.</br>
	/// If other protection bits are set, said protection bit is overrided in the page.
	/// </summary>
	OBOS_PROTECTION_SAME_AS_BEFORE = 0x10,
	/// <summary>
	/// Enables cache on the pages. This is the default.</br>
	/// Overrided by OBOS_PROTECTION_CACHE_DISABLE.
	/// </summary>
	OBOS_PROTECTION_CACHE_ENABLE = 0x20,
	/// <summary>
	/// Bits from here to OBOS_PROTECTION_PLATFORM_END are reserved for the architecture.
	/// </summary>
	OBOS_PROTECTION_PLATFORM_START = 0x01000000,
	OBOS_PROTECTION_PLATFORM_END = 0x80000000,
} prot_flags;

extern OBOS_EXPORT struct allocator_info* Mm_Allocator;

// flags: PHYS_PAGE_HUGE_PAGE
// phys32: true
extern OBOS_EXPORT page* Mm_AnonPage;
extern OBOS_EXPORT page* Mm_UserAnonPage;

OBOS_EXPORT void* MmH_FindAvailableAddress(context* ctx, size_t size, vma_flags flags, obos_status* status);
// file can be nullptr for a anonymous mapping.
OBOS_EXPORT obos_status Mm_VirtualMemoryFree(context* ctx, void* base, size_t size);
#ifndef __clang__
OBOS_EXPORT __attribute__((malloc, malloc(Mm_VirtualMemoryFree, 2))) void* Mm_VirtualMemoryAlloc(context* ctx, void* base, size_t size, prot_flags prot, vma_flags flags, fd* file, obos_status* status);
OBOS_EXPORT __attribute__((malloc, malloc(Mm_VirtualMemoryFree, 2))) void* Mm_VirtualMemoryAllocEx(context* ctx, void* base, size_t size, prot_flags prot, vma_flags flags, fd* file, uoff_t offset, obos_status* status);
#else
OBOS_EXPORT void* Mm_VirtualMemoryAlloc(context* ctx, void* base, size_t size, prot_flags prot, vma_flags flags, fd* file, obos_status* status);
OBOS_EXPORT void* Mm_VirtualMemoryAllocEx(context* ctx, void* base, size_t size, prot_flags prot, vma_flags flags, fd* file, uoff_t offset, obos_status* status);
#endif
// Note: base must be the exact address as returned by VirtualMemoryAlloc.
// isPageable values:
// 0: Non-pageable
// 1: Pageable
// >1: Same as previous value.
OBOS_EXPORT obos_status Mm_VirtualMemoryProtect(context* ctx, void* base, size_t size, prot_flags newProt, int isPageable);

// Maps user pages into the kernel address space. This can be used in syscalls to avoid copying large amounts of memory.
// returns OBOS_STATUS_PAGE_FAULT when a portion of the user memory requested is not mapped.
// NOTE: The returned address is not aligned down to the page size.
OBOS_EXPORT void* Mm_MapViewOfUserMemory(context* const user_context, void* ubase, void* kbase, size_t nBytes, prot_flags protection, bool respectUserProtection, obos_status* status);

OBOS_EXPORT obos_status Mm_VirtualMemoryLock(context* ctx, void* base, size_t sz);
OBOS_EXPORT obos_status Mm_VirtualMemoryUnlock(context* ctx, void* base, size_t sz);

OBOS_EXPORT void* Mm_AllocateKernelStack(context* target_user, obos_status* status);

// Optimized version of Mm_VirtualMemoryAlloc that allocates RW, ANON memory.
OBOS_EXPORT void* Mm_QuickVMAllocate(size_t sz, bool non_pageable);

struct physical_region
{
	uintptr_t phys;
	size_t sz;	
};

OBOS_EXPORT obos_status DrvH_ScatterGather(context* ctx, void* base, size_t size, struct physical_region** regions, size_t* nRegions, size_t maxRegionCount, bool rw);
OBOS_EXPORT obos_status DrvH_FreeScatterGatherList(context* ctx, void* base, size_t size, struct physical_region* regions, size_t nRegions);