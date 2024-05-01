/*
*	oboskrnl/arch/x86_64/entry.c
*
*	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <UltraProtocol/ultra_protocol.h>

#include <arch/x86_64/idt.h>
#include <arch/x86_64/interrupt_frame.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

#include <scheduler/thread_context_info.h>

extern void Arch_InitBootGDT();
static void test_function(uintptr_t userdata);

static char thr_stack[0x4000];
extern void Arch_disablePIC();
void Arch_KernelEntry(struct ultra_boot_context* bcontext, uint32_t magic)
{
	if (magic != ULTRA_MAGIC)
		return; // All hope is lost.
	// TODO: Parse boot context.
	// This call will ensure the IRQL is at the default IRQL (IRQL_MASKED).
	Arch_disablePIC();
	Core_GetIrql();
	asm("sti");
	OBOS_Debug("%s: Initializing the Boot GDT.\n", __func__);
	Arch_InitBootGDT();
	OBOS_Debug("%s: Initializing the Boot IDT.\n", __func__);
	Arch_InitializeIDT();
	thread_ctx ctx;
	memzero(&ctx, sizeof(ctx));
	CoreS_SetupThreadContext(&ctx, test_function, 0x45, false, thr_stack, 0x4000);
	CoreS_SwitchToThreadContext(&ctx);
	// Shouldn't ever return.
	OBOS_Log("%s: Done early boot.\n", __func__);
	while (1);
}
static void test_function(uintptr_t userdata)
{
	OBOS_Debug("In %s, userdata: %lu.\n", __func__, userdata);
	while (1);
}