/*
 * oboskrnl/arch/m68k/irq.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <scheduler/cpu_local.h>

#include <irq/irq.h>
#include <irq/irql.h>

#include <arch/m68k/asm_helpers.h>
#include <stdint.h>

uint32_t vector_base[256];
uintptr_t Arch_IRQHandlers[256];
extern void isr_stub();

void Arch_InitializeVectorTable()
{
    for (size_t i = 0; i < 256; i++)
        vector_base[i] = (uintptr_t)isr_stub;
    asm volatile("movec.l %0, %%vbr;" : :"r"(vector_base) :);
}
obos_status CoreS_RegisterIRQHandler(irq_vector_id vector, void(*handler)(interrupt_frame* frame))
{
    obos_status s = OBOS_STATUS_SUCCESS;
	if ((s = CoreS_IsIRQVectorInUse(vector)) && handler)
		return s;
	if ((uintptr_t)handler < OBOS_KERNEL_ADDRESS_SPACE_BASE && handler)
		return OBOS_STATUS_INVALID_ARGUMENT;
	Arch_IRQHandlers[vector + 0x40] = (uintptr_t)handler;
	return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status CoreS_IsIRQVectorInUse(irq_vector_id vector)
{
	if (vector > OBOS_MAX_INTERRUPT_VECTORS)
		return OBOS_STATUS_INVALID_ARGUMENT;
	return Arch_IRQHandlers[vector+0x40] ? OBOS_STATUS_IN_USE : OBOS_STATUS_SUCCESS;
}
void CoreS_SendEOI(interrupt_frame* frame)
{
    OBOS_UNUSED(frame);
}