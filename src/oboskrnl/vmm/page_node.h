/*
	oboskrnl/vmm/page_node.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vmm/pg_context.h>
#include <vmm/page_descriptor.h>

namespace obos
{
	namespace vmm
	{
		/// <summary>
		/// Represents a range of pages.
		/// </summary>
		struct page_node
		{
			page_node *next = nullptr, *prev = nullptr;
			// Is assumed by Context::RemovePageNode to be allocated by g_pdAllocator. Don't allocate with anything else.
			page_descriptor *pageDescriptors = nullptr;
			size_t nPageDescriptors = 0;
			Context* ctx = nullptr;
		};
	}
}