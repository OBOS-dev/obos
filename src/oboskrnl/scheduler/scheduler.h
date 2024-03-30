/*
	oboskrnl/scheduler/scheduler.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <allocators/slab.h>

#include <scheduler/thread.h>
#include <scheduler/ticks.h>

namespace obos
{
	namespace scheduler
	{
		constexpr uint64_t g_schedulerFrequency = 4000;
		extern allocators::SlabAllocator g_threadAllocator;
		extern ThreadList g_threadPriorities[4];
		extern SchedulerTime g_ticks;
		void schedule();
		void yield();
	}
}