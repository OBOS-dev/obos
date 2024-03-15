/*
	oboskrnl/arch/x86_64/idt.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	void InitializeIDT();
	void RawRegisterInterrupt(uint8_t vec, uintptr_t f);
}