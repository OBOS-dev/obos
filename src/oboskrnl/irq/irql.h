/*
	oboskrnl/irq/irql.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	/// <summary>
	/// Lowers the irql of the current processor.
	/// </summary>
	/// <param name="newIRQL">The new IRQL. This must be less than the current IRQL, or the function will panic.</param>
	void LowerIRQL(uint8_t newIRQL);
	/// <summary>
	/// Raises the IRQL of the current processor.
	/// </summary>
	/// <param name="newIRQL">The new IRQL. This must be greater than the current IRQL, or the function will panic.</param>
	/// <param name="oldIRQL">[out] A pointer to a variable which will store the current IRQL.</param>
	void RaiseIRQL(uint8_t newIRQL, uint8_t* oldIRQL);
	/// <summary>
	/// Gets the IRQL of the current processor.
	/// </summary>
	/// <returns>The IRQL of the current processor.</returns>
	uint8_t GetIRQL();
}