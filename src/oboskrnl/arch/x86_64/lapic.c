/*
 * oboskrnl/arch/x86_64/lapic.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <arch/x86_64/lapic.h>
#include <arch/x86_64/interrupt_frame.h>
#include <arch/x86_64/idt.h>

#include <arch/x86_64/asm_helpers.h>

#include <scheduler/cpu_local.h>

#include <mm/bare_map.h>

#define IA32_APIC_BASE 0x1B
#define APIC_BSP 0x100
#define APIC_ENABLE 0x800

obos_status Arch_MapPage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags, bool e);

lapic* Arch_LAPICAddress;
static basicmm_region lapic_region;
static void LAPIC_DefaultIrqHandler(interrupt_frame* frame)
{
	OBOS_UNUSED(frame);
	Arch_LAPICSendEOI();
}
OBOS_PAGEABLE_FUNCTION void Arch_LAPICInitialize(bool isBSP)
{
	uintptr_t lapic_msr = rdmsr(IA32_APIC_BASE);
	if (!Arch_LAPICAddress)
	{
		uintptr_t phys = lapic_msr & ~0xfff;
		Arch_LAPICAddress = (lapic*)(0xffffffffffffe000);
		Arch_MapPage(getCR3(), Arch_LAPICAddress, phys, 0x8000000000000013, false);
		lapic_region.mmioRange = true;
		OBOSH_BasicMMAddRegion(&lapic_region, Arch_LAPICAddress, 0x1000);
	}
	lapic_msr |= APIC_ENABLE;
	if (isBSP)
		lapic_msr |= APIC_BSP;
	wrmsr(IA32_APIC_BASE, lapic_msr);
	if (isBSP)
		Arch_RawRegisterInterrupt(0xfe, (uintptr_t)LAPIC_DefaultIrqHandler);
	Arch_LAPICAddress->spuriousInterruptVector = 0x1ff /* LAPIC Enabled, spurious vector 0xff */;
	Arch_LAPICAddress->lvtLINT0 = 0xfe /* Vector 0xFE, Fixed, Unmasked */;
	if (isBSP)
		Arch_LAPICAddress->lvtLINT0 |= 0x700;
	Arch_LAPICAddress->lvtLINT1 = isBSP ? 0x400 /* NMI, Unmasked */ : 0xfe;
	Arch_LAPICAddress->lvtCMCI = 0xfe /* Vector 0xFE, Fixed, Unmasked */;
	Arch_LAPICAddress->lvtError = 0xfe /* Vector 0xFE, Fixed, Unmasked */;
	Arch_LAPICAddress->lvtPerformanceMonitoringCounters = 0xfe /* Vector 0xFE, Fixed, Unmasked */;
	Arch_LAPICAddress->lvtThermalSensor = 0xfe /* Vector 0xFE, Fixed, Unmasked */;
	Arch_LAPICAddress->lvtTimer = 0xfe /* Vector 0xFE, Fixed, Unmasked */;
}
void Arch_LAPICSendEOI()
{
	OBOS_ASSERT(Arch_LAPICAddress);
	if (Arch_LAPICAddress)
		Arch_LAPICAddress->eoi = 0;
}
OBOS_NO_KASAN OBOS_NO_UBSAN obos_status Arch_LAPICSendIPI(ipi_lapic_info lapic, ipi_vector_info vector)
{
	if (!Arch_LAPICAddress)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	while (Arch_LAPICAddress->interruptCommand0_31 & (1 << 12))
		pause();
	uint64_t icr = 0x4000 /* Level: Assert */;
	if (lapic.isShorthand)
	{
		uint64_t shorthand = lapic.info.shorthand;
		if (shorthand & ~LAPIC_DESTINATION_SHORTHAND_MASK)
			return OBOS_STATUS_INVALID_ARGUMENT;
		icr |= (shorthand << 18);
	}
	else
		icr |= ((uint64_t)lapic.info.lapicId << 56);
	if (vector.deliveryMode == LAPIC_DELIVERY_MODE_FIXED)
		icr |= vector.info.vector;
	else if (vector.deliveryMode == LAPIC_DELIVERY_MODE_SIPI)
		icr |= (vector.info.address >> 12);
	icr |= ((uint64_t)vector.deliveryMode << 8);
	Arch_LAPICAddress->interruptCommand32_63 = icr >> 32;
	Arch_LAPICAddress->interruptCommand0_31 = icr & UINT32_MAX;
	while (Arch_LAPICAddress->interruptCommand0_31 & (1<<12))
		pause();
	return OBOS_STATUS_SUCCESS;
}

void CoreS_DeferIRQ(interrupt_frame* frame)
{
	Arch_LAPICSendIPI((ipi_lapic_info){.isShorthand=true,.info.shorthand=LAPIC_DESTINATION_SHORTHAND_SELF},
					  (ipi_vector_info){.deliveryMode=LAPIC_DELIVERY_MODE_FIXED,.info={.vector=frame->intNumber}});
}
