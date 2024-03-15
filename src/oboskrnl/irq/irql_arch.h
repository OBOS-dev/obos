/*
	oboskrnl/irq/irql_arch.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace arch
	{
		void SetIRQL(uint8_t newIRQL);
		uint8_t GetIRQL();
	}
}