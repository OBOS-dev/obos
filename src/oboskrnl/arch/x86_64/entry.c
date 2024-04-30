/*
*	oboskrnl/arch/x86_64/entry.c
*
*	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <UltraProtocol/ultra_protocol.h>

#include <arch/x86_64/idt.h>
#include <arch/x86_64/interrupt_frame.h>

extern void Arch_InitBootGDT();

void Arch_KernelEntry(struct ultra_boot_context* bcontext, uint32_t magic)
{
	if (magic != ULTRA_MAGIC)
		return; // All hope is lost.
	// TODO: Parse boot context.
	OBOS_Debug("Initializing the Boot GDT.\n");
	Arch_InitBootGDT();
	OBOS_Debug("Initializing the Boot IDT.\n");
	Arch_InitializeIDT();
	OBOS_Debug("Testing panics...\n");
	OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Test panic.\n");
	while (1);
}