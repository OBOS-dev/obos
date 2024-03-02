/*
	oboskrnl/vmm/init.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>

#include <vmm/init.h>

#include <allocators/slab.h>

namespace obos
{
	namespace vmm
	{
		allocators::SlabAllocator g_pgNodeAllocator;
		void InitializeVMM()
		{
			new (&g_pgNodeAllocator) allocators::SlabAllocator{};
		}
	}
}