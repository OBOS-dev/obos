/*
	oboskrnl/irq/irql.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <irq/irql.h>

#include <scheduler/thread_context_info.h>
#include <scheduler/schedule.h>
#include <scheduler/cpu_local.h>
#include <scheduler/dpc.h>

#include <utils/list.h>

irql s_irql = IRQL_MASKED;

irql* Core_GetIRQLVar()
{
	if (!CoreS_GetCPULocalPtr())
		return &s_irql;
	return &CoreS_GetCPULocalPtr()->currentIrql;
}

void Core_LowerIrql(irql to)
{
	Core_LowerIrqlNoThread(to);
	if (Core_GetCurrentThread())
		CoreS_SetThreadIRQL(&Core_GetCurrentThread()->context, to);
}
irql Core_RaiseIrql(irql to)
{
	irql oldIrql = Core_RaiseIrqlNoThread(to);
	if (Core_GetCurrentThread())
		CoreS_SetThreadIRQL(&Core_GetCurrentThread()->context, to);
	return oldIrql;
}
void Core_LowerIrqlNoThread(irql to)
{
	if (*Core_GetIRQLVar() != CoreS_GetIRQL())
		CoreS_SetIRQL(*Core_GetIRQLVar());
	OBOS_ASSERT((to & ~0xf) == 0);
	if ((to & ~0xf))
		return;
	if (to > *Core_GetIRQLVar())
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "%s: IRQL %d is greater than the current IRQL, %d.\n", __func__, to, *Core_GetIRQLVar());
	*Core_GetIRQLVar() = to;
	CoreS_SetIRQL(to);
	if (to < IRQL_DISPATCH)
	{
		CoreS_SetIRQL(IRQL_DISPATCH);
		*Core_GetIRQLVar() = IRQL_DISPATCH;
		// Run pending DPCs on the current CPU.
		for (dpc* cur = LIST_GET_HEAD(dpc_queue, &CoreS_GetCPULocalPtr()->dpcs); cur; )
		{
			dpc* next = LIST_GET_NEXT(dpc_queue, &CoreS_GetCPULocalPtr()->dpcs, cur);
			if (cur->handler(cur, cur->userdata) == cur)
				if (!LIST_IS_NODE_UNLINKED(dpc_queue, &CoreS_GetCPULocalPtr()->dpcs, cur))
					LIST_REMOVE(dpc_queue, &CoreS_GetCPULocalPtr()->dpcs, cur); // If the DPC still exists.
			cur = next;
		}
		*Core_GetIRQLVar() = to;
		CoreS_SetIRQL(to);
	}
}
irql Core_RaiseIrqlNoThread(irql to)
{
	if (*Core_GetIRQLVar() != CoreS_GetIRQL())
		CoreS_SetIRQL(*Core_GetIRQLVar());
	OBOS_ASSERT((to & ~0xf) == 0);
	if ((to & ~0xf))
		return IRQL_INVALID;
	if (to < *Core_GetIRQLVar())
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "%s: IRQL %d is less than the current IRQL, %d.\n", __func__, to, *Core_GetIRQLVar());
	irql oldIRQL = Core_GetIrql();
	CoreS_SetIRQL(to);
	*Core_GetIRQLVar() = to;
	return oldIRQL;
}
irql Core_GetIrql()
{
	if (*Core_GetIRQLVar() != CoreS_GetIRQL())
		CoreS_SetIRQL(*Core_GetIRQLVar());
	return *Core_GetIRQLVar();
}