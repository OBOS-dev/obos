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
			// This also must be in order of lowest virtual address to highest virtual address, or bad things will happen.
			// If you don't this, you'll be fine.
			struct page_descriptor *pageDescriptors = nullptr;
			size_t nPageDescriptors = 0;
			class Context* ctx = nullptr;
			allocflag_t allocFlags = 0;
			void* operator new(size_t count);
			void* operator new[](size_t count);
		};
	}
}