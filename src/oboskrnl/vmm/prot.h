/*
	oboskrnl/vmm/prot.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace vmm
	{
		typedef uint64_t prot_t;
		typedef uint64_t allocflag_t;
		enum
		{
			/// <summary>
			/// Demand that the pages are read only.
			/// </summary>
			PROT_READ_ONLY = 0x1,
			/// <summary>
			/// Allow execution of the pages.
			/// </summary>
			PROT_EXECUTE = 0x2,
			/// <summary>
			/// Allow the pages to be accessed in user mode.
			/// </summary>
			PROT_USER = 0x4,
			/// <summary>
			/// Disable cache on the allocated region.
			/// </summary>
			PROT_CACHE_DISABLE = 0x8,
			/// <summary>
			/// Disable demand paging for the allocated region. This should be used for tss/ist stacks to prevent triple faults.
			/// </summary>
			PROT_NO_DEMAND_PAGE = 0x10,
			/// <summary>
			/// The start of the range for platform-specific bits.
			/// </summary>
			PROT_PLATFORM_START = 0x0100'0000,
			/// <summary>
			/// The end of the range for platform-specific bits.
			/// </summary>
			PROT_PLATFORM_END = 0x8000'0000,
		};
		enum
		{
			/// <summary>
			/// Allocate the pages in the first 4 gib of the address space. Only valid when addr == nullptr or when FLAGS_ADDR_IS_HINT is passed. Otherwise the flag is ignored.
			/// </summary>
			FLAGS_32BIT = 0x1,
			/// <summary>
			/// Add a guard page on the left side of the allocated pages. This can be used for stacks to prevent stacks leaking into other memory.
			/// </summary>
			FLAGS_GUARD_PAGE_LEFT = 0x2,
			/// <summary>
			/// Use huge pages to allocate the memory region. The size is rounded up to the nearest huge page size.
			/// </summary>
			FLAGS_USE_HUGE_PAGES = 0x4,
			/// <summary>
			/// The address passed is a hint, and can be relocated.
			/// </summary>
			FLAGS_ADDR_IS_HINT = 0x8,
			/// <summary>
			/// Tells the kernel to not use huge pages as an optimization if there are one or more huge pages could fit in the allocated regions.
			/// </summary>
			FLAGS_DISABLE_HUGEPAGE_OPTIMIZATION = 0x10,
			/// <summary>
			/// Adds a guard page to the right side of the allocate pages.
			/// </summary>
			FLAGS_GUARD_PAGE_RIGHT = 0x20,
		};
	}
}