/*
	oboskrnl/arch/x86_64/idt.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/x86_64/interrupt_frame.h>

struct idtEntry
{
	uint16_t offset1;
	uint16_t selector;
	uint8_t ist;
	uint8_t typeAttributes;
	uint16_t offset2;
	uint32_t offset3;
	uint32_t resv1;
};

void Arch_InitializeIDT(bool isBSP);
void Arch_RawRegisterInterrupt(uint8_t vec, uintptr_t f);
void Arch_PutInterruptOnIST(uint8_t vec, uint8_t ist);