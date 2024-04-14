/*
	oboskrnl/arch/x86_64/mm/palloc.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <export.h>

namespace obos
{
	OBOS_EXPORT uintptr_t AllocatePhysicalPages(size_t nPages, bool align2MIB = false);
	OBOS_EXPORT void FreePhysicalPages(uintptr_t addr, size_t nPages);
	OBOS_EXPORT void OptimizePMMFreeList();

	OBOS_EXPORT void* MapToHHDM(uintptr_t phys);
}