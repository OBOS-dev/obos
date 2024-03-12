/*
	oboskrnl/arch/x86_64/mm/palloc.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	uintptr_t AllocatePhysicalPages(size_t nPages, bool align2MIB = false);
	void FreePhysicalPages(uintptr_t addr, size_t nPages);
	void OptimizePMMFreeList();

	void* MapToHHDM(uintptr_t phys);
}