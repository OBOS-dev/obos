/*
	oboskrnl/scheduler/scheduler.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace scheduler
	{
		using SchedulerTime = uint64_t;
		extern SchedulerTime g_ticks;
		void schedule();
	}
}