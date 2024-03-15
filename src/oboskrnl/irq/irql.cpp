/*
	oboskrnl/irq/irql.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <irq/irql_arch.h>

#include <scheduler/cpu_local.h>

namespace obos
{
	static uint8_t s_irql = 0;
	static uint8_t& getIRQLVar()
	{
		if (scheduler::GetCPUPtr())
			return scheduler::GetCPUPtr()->irql;
		return s_irql;
	}
	void LowerIRQL(uint8_t newIRQL)
	{
		OBOS_ASSERTP(arch::GetIRQL() == getIRQLVar(), "");
		newIRQL &= 0xf;
		if (newIRQL > getIRQLVar())
			logger::panic(nullptr, "Attempt to call %s() with the irql %d, which is greater than the current IRQL, %d.\n", __func__, newIRQL, s_irql);
		getIRQLVar() = newIRQL;
		arch::SetIRQL(getIRQLVar());
	}
	void RaiseIRQL(uint8_t newIRQL, uint8_t* oldIRQL)
	{
		newIRQL &= 0xf;
		OBOS_ASSERTP(arch::GetIRQL() == getIRQLVar(), "");
		if (newIRQL < getIRQLVar())
			logger::panic(nullptr, "Attempt to call %s() with the irql %d, which is less than the current IRQL, %d.\n", __func__, newIRQL, s_irql);
		*oldIRQL = getIRQLVar();
		getIRQLVar() = newIRQL;
		arch::SetIRQL(getIRQLVar());
	}
	uint8_t GetIRQL()
	{
		OBOS_ASSERTP(arch::GetIRQL() == getIRQLVar(), "");
		return getIRQLVar();
	}
}