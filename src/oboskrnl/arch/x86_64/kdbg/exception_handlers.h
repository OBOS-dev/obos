/*
	oboskrnl/arch/x86_64/kdbg/exception_handlers.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/x86_64/irq/interrupt_frame.h>

namespace obos
{
	namespace kdbg
	{
		// Returns false if the exception handler should be returned from.
		// Otherwise, true.
		bool exceptionHandler(interrupt_frame* frame);
	}

}