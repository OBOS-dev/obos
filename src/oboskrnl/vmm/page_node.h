/*
	oboskrnl/vmm/page_node.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vmm/prot.h>

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
			// This also must be in order of lowest virtual address to highest virtual address, or bad things will happen.
			// If you don't do all this, you'll be fine.
			struct page_descriptor *pageDescriptors = nullptr;
			size_t nPageDescriptors = 0;
			class Context* ctx = nullptr;
			allocflag_t allocFlags = 0;
		};
	}
}