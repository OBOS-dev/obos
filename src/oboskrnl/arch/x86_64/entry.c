/*
*	oboskrnl/arch/x86_64/entry.c
*
*	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>

#include <UltraProtocol/ultra_protocol.h>

extern void InitBootGDT();

void KernelArchInit(struct ultra_boot_context* bcontext, uint32_t magic)
{
	if (magic != ULTRA_MAGIC)
		return; // All hope is lost.
	InitBootGDT();
	while (1);
}