/*
	oboskrnl/irq/irql.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <irq/irql_arch.h>

namespace obos
{
	static uint8_t s_irql = 0;
	void LowerIRQL(uint8_t newIRQL)
	{
		OBOS_ASSERTP(arch::GetIRQL() == s_irql, "");
		newIRQL &= 0xf;
		if (newIRQL >= s_irql)
			logger::panic(nullptr, "Attempt to call %s() with the irql %d, which is greater than the current IRQL, %d.\n", __func__, newIRQL, s_irql);
		s_irql = newIRQL;
		arch::SetIRQL(s_irql);
	}
	void RaiseIRQL(uint8_t newIRQL, uint8_t* oldIRQL)
	{
		newIRQL &= 0xf;
		OBOS_ASSERTP(arch::GetIRQL() == s_irql, "");
		if (newIRQL <= s_irql)
			logger::panic(nullptr, "Attempt to call %s() with the irql %d, which is less than the current IRQL, %d.\n", __func__, newIRQL, s_irql);
		*oldIRQL = s_irql;
		s_irql = newIRQL;
		arch::SetIRQL(s_irql);
	}
	uint8_t GetIRQL()
	{
		OBOS_ASSERTP(arch::GetIRQL() == s_irql, "");
		return s_irql;
	}
}