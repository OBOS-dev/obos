/*
	oboskrnl/irq/irql.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <irq/irql.h>

irql s_irql = IRQL_MASKED;

irql* getIRQLVar()
{
	return &s_irql;
}

void Core_LowerIrql(irql to)
{
	if (*getIRQLVar() != CoreS_GetIRQL())
		CoreS_SetIRQL(*getIRQLVar());
	OBOS_ASSERT((to & ~0xf) == 0);
	if ((to & ~0xf))
		return;
	if (to > *getIRQLVar())
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "%s: IRQL %d is greater than the current IRQL.\n", __func__, to);
	*getIRQLVar() = to;
	CoreS_SetIRQL(to);
}
irql Core_RaiseIrql(irql to)
{
	if (*getIRQLVar() != CoreS_GetIRQL())
		CoreS_SetIRQL(*getIRQLVar());
	OBOS_ASSERT((to & ~0xf) == 0);
	if ((to & ~0xf))
		return IRQL_INVALID;
	if (to < *getIRQLVar())
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "%s: IRQL %d is less than the current IRQL.\n", __func__, to);
	irql oldIRQL = Core_GetIrql();
	*getIRQLVar() = to;
	CoreS_SetIRQL(to);
	return oldIRQL;
}
irql Core_GetIrql()
{
	if (*getIRQLVar() != CoreS_GetIRQL())
		CoreS_SetIRQL(*getIRQLVar());
	return *getIRQLVar();
}