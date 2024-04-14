/*
	oboskrnl/allocators/basic_allocator.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <allocators/allocator.h>

#include <locks/spinlock.h>

#define MEMBLOCK_MAGIC  0x6AB450AA
#define PAGEBLOCK_MAGIC	0x768AADFC
#define MEMBLOCK_DEAD   0x3D793CCD

namespace obos
{
	namespace allocators
	{
		// TODO: Test in userspace.
		// struct memBlock
		// {
			// alignas (0x10) uint32_t magic = MEMBLOCK_MAGIC;
			
			// alignas (0x10) size_t size = 0;
			// alignas (0x10) void* allocAddr = 0;
			
			// alignas (0x10) memBlock* next = nullptr;
			// alignas (0x10) memBlock* prev = nullptr;
			
			// alignas (0x10) struct pageBlock* pageBlock = nullptr;
		
			// alignas (0x10) void* whoAllocated = nullptr;
		// };
		// struct pageBlock
		// {
			// alignas (0x10) uint32_t magic = PAGEBLOCK_MAGIC;
			
			// alignas (0x10) pageBlock* next = nullptr;
			// alignas (0x10) pageBlock* prev = nullptr;
			
			// alignas (0x10) memBlock* firstBlock = nullptr;
			// alignas (0x10) memBlock* lastBlock = nullptr;
			// alignas (0x10) size_t nMemBlocks = 0;
			
			// struct
			// {
				// alignas (0x10) memBlock* head = nullptr;
				// alignas (0x10) memBlock* tail = nullptr;
				// alignas (0x10) size_t nBlocks = 0;
			// } freeList{};
			
			// alignas (0x10) size_t nBytesUsed = 0;
			// alignas (0x10) size_t nPagesAllocated = 0;
		// };
		class BasicAllocator final : public Allocator
		{
		public:
			struct node
			{
				alignas(0x10) uint32_t magic = MEMBLOCK_MAGIC;
				alignas(0x10) size_t size;
				alignas(0x10) void* _containingRegion;
				alignas(0x10) node *next;
				alignas(0x10) node *prev;
				constexpr void *getAllocAddr() const { return (void*)(this + 1); }
			};
			struct node_list
			{
				node *head, *tail;
				size_t nNodes;
			};
			struct region
			{
				alignas(0x10) uint32_t magic = PAGEBLOCK_MAGIC;
				alignas(0x10) size_t size;
				alignas(0x10) size_t nFreeBytes;
				alignas(0x10) node_list free, allocated;
				alignas(0x10) node* biggestFreeNode;
				
				alignas(0x10) region *next, *prev;
			};
		public:
			BasicAllocator() {}

			void* Allocate(size_t size) override;
			void* ReAllocate(void* base, size_t newSize) override;
			void Free(void* base, size_t size) override;

			size_t QueryObjectSize(const void* base) override;

			size_t GetAllocationSize() override { return 0; };

			void OptimizeAllocator() override {};

			~BasicAllocator();
		private:
			locks::SpinLock m_lock;
			region *m_regionHead = nullptr, *m_regionTail = nullptr;
			size_t m_nRegions = 0;
			size_t m_totalPagesAllocated = 0;
			region* allocateNewRegion(size_t size);
			void freeRegion(region* block);
		};
	}
}