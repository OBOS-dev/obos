/*
	oboskrnl/scheduler/init.cpp
	
	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>

#include <scheduler/init.h>
#include <scheduler/scheduler.h>
#include <scheduler/thread.h>

#include <allocators/slab.h>

namespace obos
{
	namespace scheduler
	{
		bool g_initialized;
		SchedulerTime g_ticks;
		allocators::SlabAllocator g_threadAllocator;
		uint32_t g_nextTID;
		bool InitializeScheduler()
		{
			new (&g_threadAllocator) allocators::SlabAllocator{};
			g_threadAllocator.Initialize(nullptr, sizeof(Thread), true);
			g_initialized = true;
			return true;
		}
		bool StartKernelMainThread()
		{
		}
	}
}