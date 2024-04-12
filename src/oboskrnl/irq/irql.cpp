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
	uint8_t& getIRQLVar()
	{
		if (scheduler::GetCPUPtr())
			return scheduler::GetCPUPtr()->irql;
		return s_irql;
	}
	static void setCurThreadIRQL(uint8_t to)
	{
		if (scheduler::GetCPUPtr() && scheduler::GetCPUPtr()->currentThread)
			scheduler::GetCPUPtr()->currentThread->context.irql = to;
	}
	void LowerIRQL(uint8_t newIRQL, bool setThrIRQL)
	{
		if (arch::GetIRQL() != getIRQLVar())
			arch::SetIRQL(getIRQLVar());
		newIRQL &= 0xf;
		if (newIRQL > getIRQLVar())
			logger::panic(nullptr, "Attempt to call %s() with the irql %d, which is greater than the current IRQL, %d.\n", __func__, newIRQL, getIRQLVar());
		getIRQLVar() = newIRQL;
		if (setThrIRQL)
			setCurThreadIRQL(newIRQL);
		arch::SetIRQL(getIRQLVar());
	}
	void RaiseIRQL(uint8_t newIRQL, uint8_t* oldIRQL, bool setThrIRQL)
	{
		newIRQL &= 0xf;
		if (arch::GetIRQL() != getIRQLVar())
			arch::SetIRQL(getIRQLVar());
		if (newIRQL < getIRQLVar())
			logger::panic(nullptr, "Attempt to call %s() with the irql %d, which is less than the current IRQL, %d.\n", __func__, newIRQL, getIRQLVar());
		*oldIRQL = getIRQLVar();
		getIRQLVar() = newIRQL;
		if (setThrIRQL)
			setCurThreadIRQL(newIRQL);
		arch::SetIRQL(getIRQLVar());
	}
	uint8_t GetIRQL()
	{
		if (arch::GetIRQL() != getIRQLVar())
			arch::SetIRQL(getIRQLVar());
		return getIRQLVar();
	}
}