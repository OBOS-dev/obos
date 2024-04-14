/*
	oboskrnl/arch/x86_64/asm_helpers.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	void outb(uint16_t port, uint8_t data);
	void outw(uint16_t port, uint16_t data);
	void outd(uint16_t port, uint32_t data);
	uint8_t inb(uint16_t port);
	uint16_t inw(uint16_t port);
	uint32_t ind(uint16_t port);

	uintptr_t getCR0();
	uintptr_t getCR2();
	uintptr_t getCR3();
	uintptr_t getCR4();
	uintptr_t getCR8();
	uintptr_t getEFER();
	
	uintptr_t getDR6();

	void __cpuid__(uint64_t initialEax, uint64_t initialEcx, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx);

	uint64_t rdmsr(uint32_t msr);
	void wrmsr(uint32_t msr, uint64_t val);

	void pause();
	
	void invlpg(uintptr_t addr);
	void wbinvd();
	
	void xsave(void* region);
}