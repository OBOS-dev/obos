/*
	oboskrnl/irq/irql.c

	Copyright (c) 2024-2026 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <irq/irql.h>
#include <irq/dpc.h>

#include <scheduler/thread_context_info.h>
#include <scheduler/schedule.h>
#include <scheduler/cpu_local.h>

#include <locks/spinlock.h>

#include <utils/list.h>

irql Core_TempIrql = IRQL_MASKED;

#if !OBOS_LAZY_IRQL
__attribute__((no_instrument_function)) OBOS_NO_UBSAN OBOS_NO_KASAN irql Core_RaiseIrqlNoThread(irql to)
{
	irql* irqlv = Core_GetIRQLVar();
	if (to == *irqlv)
		return to;
	if (obos_expect(to < *irqlv, false))
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "%s: IRQL %d is less than the current IRQL, %d.\n", __func__, to, *Core_GetIRQLVar());
//	if (*Core_GetIRQLVar() != CoreS_GetIRQL())
//		CoreS_SetIRQL(*Core_GetIRQLVar(), *Core_GetIRQLVar());
	irql oldIRQL = Core_GetIrql();
#if !OBOS_LAZY_IRQL
	CoreS_SetIRQL(to, *Core_GetIRQLVar());
// #else
// 	if (to == IRQL_DISPATCH)
// 		CoreS_SetIRQL(to, *Core_GetIRQLVar());
#endif
	*irqlv = to;
	return oldIRQL;
}
#endif

__attribute__((no_instrument_function)) OBOS_NO_UBSAN OBOS_NO_KASAN irql Core_RaiseIrql(irql to)
{
	irql oldIrql = Core_RaiseIrqlNoThread(to);
	if (Core_GetCurrentThread())
		CoreS_SetThreadIRQL(&Core_GetCurrentThread()->context, to);
	return oldIrql;
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN OBOS_NO_KASAN void Core_LowerIrql(irql to)
{
	if (to == *Core_GetIRQLVar())
		return;
	Core_LowerIrqlNoThread(to);
	if (Core_GetCurrentThread())
		CoreS_SetThreadIRQL(&Core_GetCurrentThread()->context, to);
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN OBOS_NO_KASAN void CoreH_DispatchDPCs()
{
	// Run pending DPCs on the current CPU.
	for (dpc* cur = LIST_GET_HEAD(dpc_queue, &CoreS_GetCPULocalPtr()->dpcs); cur; )
	{
		dpc* next = LIST_GET_NEXT(dpc_queue, &CoreS_GetCPULocalPtr()->dpcs, cur);
		LIST_REMOVE(dpc_queue, &CoreS_GetCPULocalPtr()->dpcs, cur);
		cur->cpu = nullptr;
		if (cur->handler)
			cur->handler(cur, cur->userdata);
		cur = next;
	}
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN OBOS_NO_KASAN void Core_LowerIrqlNoThread(irql to)
{
	if (to == *Core_GetIRQLVar())
		return;
	Core_LowerIrqlNoDPCDispatch(to);
	if (obos_expect(to < IRQL_DISPATCH, false))
		CoreH_DispatchDPCs();
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN OBOS_NO_KASAN void Core_LowerIrqlNoDPCDispatch(irql to)
{
	if (to != 0xff)
		OBOS_ASSERT((to & ~0xf) == 0);
	if ((to & ~0xf))
		return;

	irql* irqlv = Core_GetIRQLVar();
	if (to == *irqlv)
		return;

	// if (obos_expect(to > *irqlv, false))
		// OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "%s: IRQL %d is greater than the current IRQL, %d.\n", __func__, to, *Core_GetIRQLVar());

	uint8_t old = *irqlv;
	*irqlv = to;
	CoreS_SetIRQL(to, old);
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN OBOS_NO_KASAN irql Core_GetIrql()
{
	return *Core_GetIRQLVar();
}

#undef Core_GetIRQLVar

irql* Core_GetIRQLVar()
{
	irql* res = nullptr;
	if (obos_expect(!!CoreS_GetCPULocalPtr(), true))
		res = &CoreS_GetCPULocalPtr()->currentIrql;
	else
		res = &Core_TempIrql;
	return res;
}
