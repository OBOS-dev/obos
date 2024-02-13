/*
	oboskrnl/arch/x86_64/asm_helpers.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	uintptr_t getCR0();
	uintptr_t getCR2();
	uintptr_t getCR3();
	uintptr_t getCR4();
	uintptr_t getCR8();
	uintptr_t getEFER();
}