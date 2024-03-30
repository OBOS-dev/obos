/*
	oboskrnl/scheduler/init.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <scheduler/thread.h>

namespace obos
{
	namespace scheduler
	{
		extern bool g_initialized;
		extern ThrAffinity g_defaultAffinity;
		bool InitializeScheduler();
		bool StartKernelMainThread(void(*entry)());
	}
}