/*
	oboskrnl/arch/x86_64/pmm/alloc.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	uintptr_t AllocatePhysicalPages(size_t nPages);
	void FreePhysicalPages(uintptr_t addr, size_t nPages);
}