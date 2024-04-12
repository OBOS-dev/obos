/*
	oboskrnl/vmm/init.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/vmm_defines.h>

#include <allocators/basic_allocator.h>

// size should be page aligned or the behaviour of this macro is unpredicatable.
#define OBOS_CROSSES_PAGE_BOUNDARY(base, size) ((((uintptr_t)(base) + (size_t)(size)) % OBOS_PAGE_SIZE) == ((uintptr_t)(base) % OBOS_PAGE_SIZE))

namespace obos
{
	namespace vmm
	{
		extern class Context g_kernelContext;
		extern bool g_initialized;
		extern allocators::BasicAllocator g_vmmAllocator;
		void InitializeVMM();
	}
}