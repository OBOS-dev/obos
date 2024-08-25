/*
 * oboskrnl/mm/alloc.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <mm/context.h>

#include <allocators/base.h>

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
    VMA_FLAGS_KERNEL_STACK = VMA_FLAGS_NON_PAGED|VMA_FLAGS_NON_PAGED,
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
	/// For Mm_VirtualMemoryProtect. Sets the protection to the same thing it was before.
	/// </summary>
	OBOS_PROTECTION_SAME_AS_BEFORE = 0x10,
	/// <summary>
	/// Bits from here to OBOS_PROTECTION_PLATFORM_END are reserved for the architecture.
	/// </summary>
	OBOS_PROTECTION_PLATFORM_START = 0x01000000,
	OBOS_PROTECTION_PLATFORM_END = 0x80000000,
} prot_flags;

extern allocator_info* Mm_Allocator;

OBOS_EXPORT void* MmH_FindAvailableAddress(context* ctx, size_t size, vma_flags flags, obos_status* status);
// file can be nullptr for a anonymous mapping.
OBOS_EXPORT void* Mm_VirtualMemoryAlloc(context* ctx, void* base, size_t size, prot_flags prot, vma_flags flags, fd* file, obos_status* status);
// Note: base must be the exact address as returned by AllocateVirtualMemory.
OBOS_EXPORT obos_status Mm_VirtualMemoryFree(context* ctx, void* base, size_t size);
// Note: base must be the exact address as returned by AllocateVirtualMemory.
// isPageable values:
// 0: Non-pageable
// 1: Pageable
// >1: Same as previous value.
OBOS_EXPORT obos_status Mm_VirtualMemoryProtect(context* ctx, void* base, size_t size, prot_flags newProt, int isPageable);
