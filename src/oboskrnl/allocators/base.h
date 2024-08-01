/*
 * oboskrnl/allocators/base.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

// Defines a struct that defines the base info and interfaces for an allocator.

typedef struct allocator_info
{
	/// <summary>
	/// Identifies the allocator used.
	/// </summary>
	uint64_t magic;
	/// <summary>
	/// Allocates memory.
	/// </summary>
	/// <param name="This">The allocator object used.</param>
	/// <param name="nBytes">The amount of bytes to allocate.</param>
	/// <param name="status">[out,optional] The status code of the function.</param>
	/// <returns>A pointer to the block allocated, or nullptr on failure.</returns>
	void* (*Allocate)(struct allocator_info* This, size_t nBytes, obos_status* status);
	/// <summary>
	/// Does the same as Allocate, except it also zeroes the memory block returned.
	/// </summary>
	/// <param name="This">The allocator object used.</param>
	/// <param name="nObjects">The amount of objects to allocate.</param>
	/// <param name="bytesPerObject">The size of each object.</param>
	/// <param name="status">[out,optional] The status code of the function.</param>
	/// <returns>A pointer to the block allocated, or nullptr on failure.</returns>
	void* (*ZeroAllocate)(struct allocator_info* This, size_t nObjects, size_t bytesPerObject, obos_status* status);
	/// <summary>
	/// Reallocates a previously allocated block of memory.<para/>
	/// If nBytes==0, the function shall call Free() on the block.<para/>
	/// If base==nullptr, the function shall Allocate() the byte count passed.
	/// </summary>
	/// <param name="This">The allocator object used.</param>
	/// <param name="base">The base of the other allocated block of memory.</param>
	/// <param name="nBytes">The new byte count.</param>
	/// <param name="status">[out,optional] The status code of the function.</param>
	/// <returns>The new block, or nullptr on failure. This function is allowed to return the same base passed.</returns>
	void* (*Reallocate)(struct allocator_info* This, void* base, size_t nBytes, obos_status* status);
	/// <summary>
	/// Frees a previously allocated block of memory.
	/// </summary>
	/// <param name="This">The allocator object used.</param>
	/// <param name="base">The base of the memory</param>
	/// <param name="nBytes">The size of the memory block (could be optional depending on the allocator)</param>
	/// <returns>The status of the function.</returns>
	obos_status (*Free)(struct allocator_info* This, void* base, size_t nBytes);
	/// <summary>
	/// Queries the size of an allocated block.
	/// </summary>
	/// <param name="This">The allocator object used.</param>
	/// <param name="base">The base of the region.</param>
	/// <param name="nBytes">[out] The size of the block.</param>
	/// <returns>The status of the function.</returns>
	obos_status(*QueryBlockSize)(struct allocator_info* This, void* base, size_t* nBytes);
} allocator_info;
extern OBOS_EXPORT allocator_info* OBOS_KernelAllocator;
extern OBOS_EXPORT allocator_info* OBOS_NonPagedPoolAllocator;