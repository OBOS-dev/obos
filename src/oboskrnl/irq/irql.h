/*
	oboskrnl/irq/irql.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	// For internal-use only.
	uint8_t& getIRQLVar();
	/// <summary>
	/// Lowers the irql of the current processor.
	/// </summary>
	/// <param name="newIRQL">The new IRQL. This must be less than the current IRQL, or the function will panic.</param>
	/// <param name="setThrIRQL">[opt] Whether to set the current thread's IRQL in the context..</param>
	void LowerIRQL(uint8_t newIRQL, bool setThrIRQL = true);
	/// <summary>
	/// Raises the IRQL of the current processor.
	/// </summary>
	/// <param name="newIRQL">The new IRQL. This must be greater than the current IRQL, or the function will panic.</param>
	/// <param name="oldIRQL">[out] A pointer to a variable which will store the current IRQL.</param>
	/// <param name="setThrIRQL">[opt] Whether to set the current thread's IRQL in the context..</param>
	void RaiseIRQL(uint8_t newIRQL, uint8_t* oldIRQL, bool setThrIRQL = true);
	/// <summary>
	/// Gets the IRQL of the current processor.
	/// </summary>
	/// <returns>The IRQL of the current processor.</returns>
	uint8_t GetIRQL();
	// IRQL table:
	// 0: All IRQs are allowed to run. You cannot use this to initialize an Irq object.
	// 1: Invalid.
	// 2: The timer for the scheduler is disallowed to run at this IRQL and higher.
	// 3: IPIs aren't processed at this level or higher, the IRQL must be lowered to handle IPIs.
	// 4: IRQL for ACPI General-Purpose Events. This is only to ever be used by ACPI IRQs, or bad stuff could happen.
	// 5-14: Unused by the kernel or drivers.
	// 15: All IRQs are masked. Otherwise unused by drivers and the kernel.
	enum
	{
		IRQL_PASSIVE = 0,
		IRQL_DISPATCH = 2,
		IRQL_IPI_DISPATCH,
		IRQL_GPE,
		IRQL_MASK_ALL = 15,
		IRQL_INVALID = 0xff,
	};
}