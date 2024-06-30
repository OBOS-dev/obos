/*
 * oboskrnl/arch/x86_64/idt.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include "mm/context.h"
#include <int.h>
#include <error.h>
#include <struct_packing.h>

#include <irq/irq.h>

#include <arch/x86_64/idt.h>
#include <arch/x86_64/lapic.h>
#include <arch/x86_64/irq_vector.h>

#include <arch/x86_64/asm_helpers.h>

extern char Arch_b_isr_handler;
extern char Arch_e_isr_handler;

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
OBOS_EXCLUDE_VAR_FROM_MM struct idtEntry g_idtEntries[256];
struct idtPointer
{
	uint16_t size;
	uintptr_t idt;
} OBOS_PACK;
enum
{
	// Present, Interrupt Gate
	DEFAULT_TYPE_ATTRIBUTE = 0x8E,
	// Max DPL: 3
	TYPE_ATTRIBUTE_USER_MODE = 0x60
};
OBOS_EXCLUDE_VAR_FROM_MM uintptr_t Arch_IRQHandlers[256];
extern void Arch_FlushIDT(struct idtPointer* idtptr);
static void RegisterISRInIDT(uint8_t vec, uintptr_t addr, bool canUsermodeCall, uint8_t ist)
{
	struct idtEntry idtEntry = g_idtEntries[vec];
	idtEntry.ist = ist & 0x7;
	idtEntry.offset1 = addr & 0xffff;
	idtEntry.selector = 0x8;
	idtEntry.typeAttributes = DEFAULT_TYPE_ATTRIBUTE | (canUsermodeCall ? TYPE_ATTRIBUTE_USER_MODE : 0);
	idtEntry.offset2 = (addr >> 16) & 0xffff;
	idtEntry.offset3 = (addr >> 32) & 0xffffffff;
	g_idtEntries[vec] = idtEntry;
}
static int getIntIST(int i)
{
	if (i > 32)
		return 0;
	if (i == 8)
		return 2;
	return i == 2 ? 1 : 0;
}
void Arch_InitializeIDT(bool isBSP)
{
	if (isBSP)
	{
		for (int i = 0; i < 256; i++)
			RegisterISRInIDT(i,
							 (uintptr_t)(&Arch_b_isr_handler + (i * 32)),
							 i == 3 /* The only interrupt that can be called from user mode is the breakpoint interrupt */, 
							 getIntIST(i));
	}
	struct idtPointer idtPtr;
	idtPtr.size = sizeof(g_idtEntries) - 1;
	idtPtr.idt = (uintptr_t)g_idtEntries;
	Arch_FlushIDT(&idtPtr);
}
void Arch_RawRegisterInterrupt(uint8_t vec, uintptr_t f)
{
	Arch_IRQHandlers[vec] = f;
}
void Arch_PutInterruptOnIST(uint8_t vec, uint8_t ist)
{
	if (ist > 8)
		return;
	g_idtEntries[vec].ist = ist;
}
obos_status CoreS_RegisterIRQHandler(irq_vector_id vector, void(*handler)(interrupt_frame* frame))
{
	obos_status s = OBOS_STATUS_SUCCESS;
	if ((s = CoreS_IsIRQVectorInUse(vector)) && handler)
		return s;
	if(!(((uintptr_t)(handler) >> 47) == 0 || ((uintptr_t)(handler) >> 47) == 0x1ffff))
		return OBOS_STATUS_INVALID_ARGUMENT;
	if ((uintptr_t)handler < OBOS_KERNEL_ADDRESS_SPACE_BASE && handler)
		return OBOS_STATUS_INVALID_ARGUMENT;
	Arch_IRQHandlers[vector + 32] = (uintptr_t)handler;
	return OBOS_STATUS_SUCCESS;
}
obos_status CoreS_IsIRQVectorInUse(irq_vector_id vector)
{
	if (vector > 224)
		return OBOS_STATUS_INVALID_ARGUMENT;
	return Arch_IRQHandlers[vector+32] ? OBOS_STATUS_IN_USE : OBOS_STATUS_SUCCESS;
}
void CoreS_SendEOI(interrupt_frame* unused)
{
	Arch_LAPICSendEOI();
}
void CoreS_EnterIRQHandler(interrupt_frame* frame) { sti(); }
void CoreS_ExitIRQHandler(interrupt_frame* frame) { cli(); }