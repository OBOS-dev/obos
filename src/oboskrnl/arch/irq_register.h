#pragma once

#if defined(__x86_64__) || defined(_WIN64)
#include <arch/x86_64/irq/idt.h>
#include <arch/x86_64/irq/interrupt_frame.h>
namespace obos
{
	namespace arch
	{
		extern void SendEOI(interrupt_frame*);
		inline bool IsSpuriousInterrupt(interrupt_frame*) { return false; } // We can assume no, as spurious interrupts are sent through the spurious interrupt vector of the LAPIC.
	}
}
#endif

#ifndef OBOS_MAX_INTERRUPT_VECTORS
#error OBOS_MAX_INTERRUPT_VECTORS is not defined by the architecture
#endif
#ifndef OBOS_MAX_INTERRUPT_VECTORS_PER_IRQL
#error OBOS_MAX_INTERRUPT_VECTORS_PER_IRQL is not defined by the architecture
#endif
#ifndef OBOS_IRQL_TO_VECTOR
#include <todo.h>
COMPILE_MESSAGE("OBOS_IRQL_TO_VECTOR is not defined. Defining automatically...\n");
#define OBOS_IRQL_TO_VECTOR(irql) ((irql) >= 2 ? ((irql) * OBOS_MAX_INTERRUPT_VECTORS_PER_IRQL) : 0)
#endif
#ifndef OBOS_NO_EOI_ON_SPURIOUS_INTERRUPT
COMPILE_MESSAGE("OBOS_NO_EOI_ON_SPURIOUS_INTERRUPT is not defined. Setting to true.\n");
#define OBOS_NO_EOI_ON_SPURIOUS_INTERRUPT (true)
#endif
static_assert(OBOS_MAX_INTERRUPT_VECTORS_PER_IRQL < OBOS_MAX_INTERRUPT_VECTORS, "");
static_assert((OBOS_MAX_INTERRUPT_VECTORS / OBOS_MAX_INTERRUPT_VECTORS_PER_IRQL) != 0, "");