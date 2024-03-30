/*
	oboskrnl/arch/x86_64/idt.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/x86_64/irq/interrupt_frame.h>

#define OBOS_MAX_INTERRUPT_VECTORS (224)
#define OBOS_MAX_INTERRUPT_VECTORS_PER_IRQL (0x10)
#define OBOS_IRQL_TO_VECTOR(irql) ((irql) >= 2 ? ((irql) * 0x10 - 0x20) : 0)
#define OBOS_NO_EOI_ON_SPURIOUS_INTERRUPT (true)

namespace obos
{
	void InitializeIDT();
	void RawRegisterInterrupt(uint8_t vec, uintptr_t f);
	namespace arch
	{
		/// <summary>
		/// Registers an interrupt handler.
		/// </summary>
		/// <param name="vec">The vector. If the interrupt at this vector has already been registered, the previous handler will be overwritten.</param>
		/// <param name="f">The handler. This can be nullptr to unregister the interrupt handler.</param>
		void RegisterInterrupt(uint8_t vec, void(*f)(interrupt_frame*));
		/// <summary>
		/// Queries whether an interrupt has been registered.
		/// </summary>
		/// <param name="vec">The interrupt's vector.</param>
		/// <returns>Whether the interrupt has been registered.</returns>
		bool InterruptRegistered(uint8_t vec);
	}
}