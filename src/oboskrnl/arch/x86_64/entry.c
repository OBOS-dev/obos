/*
*	oboskrnl/arch/x86_64/entry.c
*
*	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>

#include <UltraProtocol/ultra_protocol.h>

#include <arch/x86_64/idt.h>
#include <arch/x86_64/interrupt_frame.h>

extern void Arch_InitBootGDT();

void int3_handler(interrupt_frame* frame)
{

}

void Arch_KernelEntry(struct ultra_boot_context* bcontext, uint32_t magic)
{
	if (magic != ULTRA_MAGIC)
		return; // All hope is lost.
	Arch_InitBootGDT();
	Arch_InitializeIDT();
	Arch_RawRegisterInterrupt(3, int3_handler);
	asm("int3");
	while (1);
}