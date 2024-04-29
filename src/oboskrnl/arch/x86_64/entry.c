/*
*	oboskrnl/arch/x86_64/entry.c
*
*	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>

#include <UltraProtocol/ultra_protocol.h>

void KernelArchInit(struct ultra_boot_context* bcontext, uint32_t magic)
{
	if (magic != ULTRA_MAGIC)
		return; // All hope is lost.
	while (1);
}