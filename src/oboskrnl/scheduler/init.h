/*
	oboskrnl/scheduler/init.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace scheduler
	{
		extern bool g_initialized;
		bool InitializeScheduler();
		bool StartKernelMainThread();
	}
}