/*
	oboskrnl/vmm/init.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <allocators/slab.h>

#include <arch/vmm_defines.h>

// size should be page aligned or the behaviour of this macro is unpredicatable.
#define OBOS_CROSSES_PAGE_BOUNDARY(base, size) ((((uintptr_t)(base) + (size_t)(size)) % OBOS_PAGE_SIZE) == ((uintptr_t)(base) % OBOS_PAGE_SIZE))

namespace obos
{
	namespace vmm
	{
		extern allocators::SlabAllocator g_pgNodeAllocator;
		extern allocators::SlabAllocator g_pdAllocator;
		extern class Context g_kernelContext;
		extern bool g_initialized;
		void InitializeVMM();
	}
}