/*
	oboskrnl/vmm/init.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <allocators/slab.h>

namespace obos
{
	namespace vmm
	{
		extern allocators::SlabAllocator g_pgNodeAllocator;
		extern allocators::SlabAllocator g_pdAllocator;
		extern class Context g_kernelContext;
		void InitializeVMM();
	}
}