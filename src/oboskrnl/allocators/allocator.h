/*
	oboskrnl/allocators/allocator.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace allocators
	{
		// Defines the general structure of an allocator.
		class Allocator
		{
		public:
			Allocator() = default;

			/// <summary>
			/// Allocates memory.
			/// </summary>
			/// <param name="size">The size of the region. If the allocator is fixed-size, this should be treated as the count of objects to allocate.</param>
			/// <returns>A pointer to the block of memory allocated, or nullptr on failure.</returns>
			virtual void* Allocate(size_t size) = 0;
			/// <summary>
			/// Allocates memory, then zeroes it.
			/// </summary>
			/// <param name="size">The size of the region. Refer to Allocate for more information.</param>
			/// <returns>A pointer to the block of memory allocated, or nullptr on failure.</returns>
			virtual void* ZeroAllocate(size_t size);
			/// <summary>
			/// Reallocates a region of memory. The allocator is not required to support this. 
			/// </summary>
			/// <param name="base">The base of the region.</param>
			/// <param name="newSize">The new size. Refer to Allocate() for more information.</param>
			/// <returns>A pointer to the new block. The function returns nullptr on failure, any other pointer should be assumed to be the new block, even if it is the same passed to the function.</returns>
			virtual void* ReAllocate(void* base, size_t newSize) { (base = base); (newSize = newSize); return nullptr; };
			/// <summary>
			/// Frees a region of memory. It is undefined behaviour to use the region after it is freed, and should not be done at all.
			/// </summary>
			/// <param name="base">The base of the region to free.</param>
			/// <param name="size">The size of the region to free. Refer to Allocate() for more information. This can be zero if the allocator doesn't need the size to free objects, but it would be better if you always pass in the size. If you don't know the size, use QueryObjectSize.</param>
			virtual void Free(void* base, size_t size) = 0;

			/// <summary>
			/// Queries the size of an object. This does not need to be the same size passed to Allocate, as the allocator might add padding.
			/// </summary>
			/// <param name="base">The object.</param>
			/// <returns>On success, see the size parameter of Allocate() for information on the return value. On failure, it returns SIZE_MAX.</returns>
			virtual size_t QueryObjectSize(const void* base) = 0;

			/// <summary>
			/// Gets the allocation size of the allocator.
			/// </summary>
			/// <returns>The allocation size, or zero if the allocator does not use fixed-sized allocations.</returns>
			virtual size_t GetAllocationSize() = 0;

			/// <summary>
			/// Optimizes the allocator's data structures, so that it runs faster, takes less memory, or has a higher success rate of providing a free block of memory.<para></para>
			/// This can, for example, in a free list allocator, make it so that all continuous free regions are continuous in the free list.
			/// </summary>
			virtual void OptimizeAllocator() {};

			virtual void* operator()(size_t size = 1) { return Allocate(size); }

			virtual ~Allocator();
		};
		extern Allocator* g_kAllocator;
	}
}