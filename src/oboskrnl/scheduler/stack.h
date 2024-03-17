/*
	oboskrnl/scheduler/stack.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace scheduler
	{
		// Rearranging/changing the alignment of this struct might cause some problems.
		// Just don't.
		struct thr_stack
		{
			alignas(0x10) uintptr_t base;
			alignas(0x10) size_t size;
		};
	}
}