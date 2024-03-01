/*
	oboskrnl/vmm/map.h

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
		/// Allocates and maps 'size' pages at 'where'.
		/// </summary>
		/// <param name="where">Where to allocate the pages at. This is rounded down to the nearest page size before being used. This cannot be nullptr.</param>
		/// <param name="size">The size of the region to map. This is rounded up to the nearest page size.</param>
		/// <param name="flags">The allocation flags.</param>
		/// <param name="protection">The protection flags of the pages.</param>
		/// <returns>'where', but rounded down, or nullptr is where (rounded down), or size (rounded up) are zero.</returns>
		void* RawAllocate(void* where, size_t size, allocflag_t flags, prot_t protection);
		/// <summary>
		/// Frees (un-maps) a region of pages.<para></para>
		/// If part of the region is un-mapped, the function fails.
		/// </summary>
		/// <param name="where">The base address to un-map.</param>
		/// <param name="size">The size of the region to free. This is rounded up to the nearest page size.<para></para></param>
		/// <returns>Whether the function succeeded (true) or not (false).</returns>
		bool RawFree(void* where, size_t size);

		/// <summary>
		/// (Re)Maps a page descriptor.
		/// </summary>
		/// <param name="ctx">The context to map as. This cannot be nullptr.</param>
		/// <param name="pd">The page descriptor to map.</param>
		/// <returns>'true' on success, otherwise false. Panics if context == nullptr, and debug mode is enabled.
		/// If context == nullptr, and OBOS_RELEASE is defined, the function returns false.</returns>
		bool MapPageDescriptor(Context* ctx, const page_descriptor& pd);
		/// <summary>
		/// Allocates and maps pages at 'base'.
		/// </summary>
		/// <param name="ctx">The context to allocate as. This cannot be nullptr.<para></para></param>
		/// <param name="base">The base of the allocation. This can be nullptr to tell the kernel to find the address.
		/// If flags has FLAGS_ADDR_IS_HINT set, the base is ignored if the region is already mapped, and another region is found.<para></para></param>
		/// <param name="size">The size of the region to map. This is rounded up to the nearest page size.<para></para></param>
		/// <param name="flags">The allocation flags.<para></para></param>
		/// <param name="protection">The protection flags of the pages.<para></para></param>
		/// <returns>The base of the allocation, or nullptr on failure.</returns>
		void *Allocate(Context* ctx, void* base, size_t size, allocflag_t flags, prot_t protection);
		/// <summary>
		/// Frees (un-maps) a region of pages.<para></para>
		/// If part of the region is un-mapped, the function fails.
		/// </summary>
		/// <param name="ctx">The context to un-map as. This cannot be nullptr.<para></para></param>
		/// <param name="base">The base address to un map.</param>
		/// <param name="size">The size of the region to free. This is rounded up to the nearest page size.<para></para></param>
		/// <returns>Whether the function succeeded in it's operation (true) or not (false).</returns>
		bool Free(Context *ctx, void* base, size_t size);
	}
}