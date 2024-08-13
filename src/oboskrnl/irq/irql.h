/*
	oboskrnl/irq/irql.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

// You mustn't include anything else, or everything will likely stop compiling.
#include <int.h>

typedef uint8_t irql;
enum
{
	IRQL_PASSIVE,
#if OBOS_IRQL_COUNT == 16
	IRQL_DISPATCH = 2,
	IRQL_TIMER = 3,
	IRQL_MASKED = 0xf,
#elif OBOS_IRQL_COUNT == 8
	IRQL_DISPATCH = 1,
	IRQL_TIMER = 1,
	IRQL_MASKED = 7,
#elif OBOS_IRQL_COUNT == 4
	IRQL_DISPATCH = 1,
	IRQL_TIMER = 1,
	IRQL_MASKED = 3,
#elif OBOS_IRQL_COUNT == 2
	IRQL_DISPATCH = 0,
	IRQL_TIMER = 0,
	IRQL_MASKED = 1,
#else
#	error Your IRQLs are too powerful for OBOS!
#endif
	IRQL_INVALID = 0xff,
};

/// <summary>
/// Lowers the IRQL. Panics if 'to' > Core_GetIrql().
/// </summary>
/// <param name="to">The new irql.</param>
OBOS_EXPORT void Core_LowerIrql(irql to);
/// <summary>
/// Raises the IRQL. Panics if 'to' < Core_GetIrql().
/// </summary>
/// <param name="to">The new irql.</param>
/// <returns>The old irql.</returns>
OBOS_NODISCARD_REASON("You must save the return value of Core_RaiseIrql to be passed to Core_LowerIrql later on.")
OBOS_EXPORT irql Core_RaiseIrql(irql to);
// Does the same as Core_LowerIrql, except it doesn't use CoreS_SetThreadIrql.
// For internal use only.
OBOS_EXPORT void Core_LowerIrqlNoThread(irql to);
// Does the same as Core_RaiseIrql, except it doesn't use CoreS_SetThreadIrql.
// For internal use only.
OBOS_NODISCARD_REASON("You must save the return value of Core_RaiseIrqlNoThread to be passed to Core_LowerIrqlNoThread later on.")
OBOS_EXPORT irql Core_RaiseIrqlNoThread(irql to);
/// <summary>
/// Gets the current IRQL.
/// </summary>
/// <returns>The current IRQL.</returns>
OBOS_EXPORT irql Core_GetIrql();
// These functions are arch-specific.
// Do not use unless you know what you're doing.

/// <summary>
/// Sets the current IRQL in the IRQ controller.<para/>
/// For example, this would set the cr8 register to 'to' on x86_64.
/// </summary>
/// <param name="to">The IRQL to set the current IRQL to.</param>
/// <param name="old">The old IRQL.</param>
OBOS_WEAK void CoreS_SetIRQL(uint8_t to, uint8_t old);
/// <summary>
/// Sets the current IRQL in the IRQ controller.<para/>
/// For example, this would return the cr8 register on x86_64.
/// </summary>
/// <returns>The current IRQL.</returns>
OBOS_WEAK uint8_t CoreS_GetIRQL();