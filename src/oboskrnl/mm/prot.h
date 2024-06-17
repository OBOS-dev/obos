/*
 * oboskrnl/mm/prot.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>

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
	/// Bits from here to OBOS_PROTECTION_PLATFORM_END are reserved for the platform.
	/// </summary>
	OBOS_PROTECTION_PLATFORM_START = 0x01000000,
	OBOS_PROTECTION_PLATFORM_END = 0x80000000,
} prot_flags;