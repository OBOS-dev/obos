/*
	oboskrnl/scheduler/cpu_local.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <scheduler/stack.h>
#include <scheduler/thread.h>

#include <arch/smp_cpu_local.h>

namespace obos
{
	namespace scheduler
	{
		struct cpu_local
		{
			// Moving this member might cause some problems.
			// Just don't.
			thr_stack startupStack{};
			uint8_t cpuId{};
			thr_stack tempStack{};
			bool isBSP{};
			bool initialized{};
			arch::cpu_local_arch archSpecific{};
			uint8_t irql{};
			Thread* volatile currentThread;
			ThreadList dpcList;
		};
		cpu_local* GetCPUPtr();
		extern cpu_local* g_cpuInfo;
		extern size_t g_nCPUs;
	}
}