/*
	oboskrnl/arch/x86_64/sched_timer.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <scheduler/cpu_local.h>

#include <irq/irq.h>

namespace obos
{
	namespace arch
	{
		void StartTimerOnCPU(scheduler::cpu_local* cpu, uint64_t freqHz, Irq& irq);
	}
}