/*
	oboskrnl/vmm/page_fault_reason.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace vmm
	{
		enum class PageFaultReason
		{
			/// <summary>
			/// When a page that was marked as demand paging (and hasn't been allocated yet) faults.
			/// </summary>
			PageFault_DemandPaging,
			/// <summary>
			/// A plain page fault happened; one that has was caused by an invalid address being accessed.
			/// </summary>
			PageFault_AccessViolation,
		};
		// Bit field.
		enum PageFaultErrorCode
		{
			/// <summary>
			/// A page fault happened while reading. This cannot be combined with PageFault_Write.
			/// </summary>
			PageFault_Read = 0x1,
			/// <summary>
			/// A page fault happened while writing. This cannot be combined with PageFault_Read.
			/// </summary>
			PageFault_Write = 0x2,
			/// <summary>
			/// A page fault happened while executing pages. This cannot be combined with PageFault_Write.
			/// </summary>
			PageFault_Execution = 0x4,
			/// <summary>
			/// A page fault happened when a page that was marked as demand paging (and hasn't been allocated yet) was accessed.
			/// </summary>
			PageFault_DemandPage = 0x8,
			/// <summary>
			/// Whether the page accessed was present (bit set) or not (bit clear).
			/// </summary>
			PageFault_IsPresent = 0x10,
			/// <summary>
			/// Whether the page fault occurred in user mode (bit set) or not (bit clear).
			/// </summary>
			PageFault_InUserMode = 0x20,
		};
	}
}