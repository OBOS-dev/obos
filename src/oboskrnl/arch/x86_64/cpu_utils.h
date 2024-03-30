/*
	oboskrnl/arch/x86_64/cpu_utils.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace arch
	{
		// x86-64 specific.
		extern uint8_t g_lapicIDs[256];
		
		// All architectures should define these next two functions.

		/// <summary>
		/// Starts all processors. This cannot be used to restart CPUs after a call to StopCPUs().
		/// </summary>
		/// <returns>The amount of processors on the system.</returns>
		size_t StartProcessors();
		/// <summary>
		/// Stops all CPUs.
		/// </summary>
		/// <param name="includingSelf">Whether to stop the current cpu as well. If true, this function doesn't return.</param>
		void StopCPUs(bool includingSelf = false);
	}
}