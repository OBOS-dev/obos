/*
	oboskrnl/allocators/slab_structs.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

namespace obos
{
	namespace allocators
	{
#ifndef OBOS_INITIAL_SLAB_COUNT
#define OBOS_INITIAL_SLAB_COUNT 32
#endif
		struct SlabNode
		{
			SlabNode *next, *prev;
			size_t size;
			// Moving this variable in the structure "may" break things.
			char* data;
		} OBOS_ALIGN(8);
		struct SlabList
		{
			SlabNode *head, *tail;
			size_t nNodes;
			void Append(SlabNode* node);
			void Remove(SlabNode* node);
		};
	}
}