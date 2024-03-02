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
		void InitializeVMM();
	}
}