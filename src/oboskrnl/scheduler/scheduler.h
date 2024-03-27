/*
	oboskrnl/scheduler/scheduler.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <allocators/slab.h>

namespace obos
{
	namespace scheduler
	{
		extern allocators::SlabAllocator g_threadAllocator;
		using SchedulerTime = uint64_t;
		extern SchedulerTime g_ticks;
		void schedule();
	}
}