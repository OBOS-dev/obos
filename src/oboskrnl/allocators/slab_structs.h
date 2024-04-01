/*
	oboskrnl/allocators/slab_structs.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

#include <locks/spinlock.h>

#define SLAB_NODE_MAGIC 0x78287c08b8ef8b4d
#define SLAB_REGION_NODE_MAGIC 0xc500f550a3ddd2e7

#ifndef OBOS_INITIAL_SLAB_COUNT
#define OBOS_INITIAL_SLAB_COUNT 32
#endif

namespace obos
{
	namespace allocators
	{
		struct SlabNode
		{
			SlabNode() = default;
			uint64_t magic = SLAB_NODE_MAGIC;
			SlabNode *next = nullptr, *prev = nullptr;
			size_t size = 0;
			char* data = nullptr;
		};
		struct SlabList
		{
			SlabNode *head, *tail;
			size_t nNodes;
			void Append(SlabNode* node);
			void Remove(SlabNode* node);
		};
		struct SlabRegionNode
		{
			uint64_t magic = SLAB_REGION_NODE_MAGIC;
			void* base = nullptr;
			size_t regionSize = 0;
			SlabList freeNodes = {};
			SlabList allocatedNodes = {};
			locks::SpinLock lock = {};

			SlabRegionNode *next = nullptr, *prev = nullptr;
		};
		struct SlabRegionList
		{
			SlabRegionNode *head, *tail;
			size_t nNodes;
			void Append(SlabRegionNode* node);
			void Remove(SlabRegionNode* node);
		};
	}
}