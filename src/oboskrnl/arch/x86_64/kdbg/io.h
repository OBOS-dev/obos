/*
	oboskrnl/arch/x86_64/kdbg/io.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <klog.h>

namespace obos
{
	namespace kdbg
	{
		enum class input_format
		{
			INVALID,
			PS2_KEYBOARD,
			SERIAL,
		};
		enum class output_format
		{
			INVALID,
			CONSOLE,
			SERIAL,
		};
		constexpr char EOF = -1;
		extern input_format g_inputDev;
		extern output_format g_outputDev;
		// Returns EOF when no character was inputted, and async is true.
		char getchar(bool async = false, bool echo = true);
		bool putchar(char ch, bool async = false);
		
		FORMAT(printf, 1) size_t printf(const char* format, ...);
		char* getline();
	}

}