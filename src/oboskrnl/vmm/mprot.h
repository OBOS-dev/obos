/*
	oboskrnl/vmm/mprot.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vmm/pg_context.h>
#include <vmm/page_descriptor.h>
#include <vmm/prot.h>

namespace obos
{
	namespace vmm
	{
		/// <summary>
		/// Sets the protection of a region of pages.<para></para>
		/// The region must be at least reserved.
		/// </summary>
		/// <param name="ctx">The context to set the protection as. This cannot be nullptr.</param>
		/// <param name="base">The base of the region.</param>
		/// <param name="size">The size of the region.</param>
		/// <param name="protection">The new protection.</param>
		/// <returns>Whether the function succeeded (true) or not (false).</returns>
		bool SetProtection(Context* ctx, void* base, size_t size, prot_t protection);
		/// <summary>
		/// Gets the page descriptors of a region of pages.
		/// </summary>
		/// <param name="ctx">The context to get the descriptors as. This cannot be nullptr.</param>
		/// <param name="base">The base of the region.</param>
		/// <param name="size">The size of the region. This is rounded up to the nearest page size.<para></para></param>
		/// <param name="oArr">[out] The array of page descriptors.</param>
		/// <param name="maxElements">The maximum number of page descriptors that can be stored in the array. If this threshold is reached, the function stops filling oArr.</param>
		/// <returns>How much more entries are needed for all pages, or SIZE_MAX on failure.</returns>
		size_t GetPageDescriptor(Context* ctx, void* base, size_t size, page_descriptor* oArr, size_t maxElements);
	}
}