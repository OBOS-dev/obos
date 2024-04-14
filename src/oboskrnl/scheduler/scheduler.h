/*
	oboskrnl/scheduler/scheduler.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <export.h>

#include <scheduler/thread.h>
#include <scheduler/ticks.h>

namespace obos
{
	namespace scheduler
	{
		constexpr uint64_t g_schedulerFrequency = 4000;
		extern ThreadList g_threadPriorities[4];
		extern SchedulerTime g_ticks;
		void schedule();
		OBOS_EXPORT void yield();
	}
}