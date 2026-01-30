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
#define IA32_X2APIC_REGISTERS 0x800

obos_status Arch_MapPage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags, bool e);

lapic* Arch_LAPICAddress;
static bool has_x2apic;
static basicmm_region lapic_region;
static void LAPIC_DefaultIrqHandler(interrupt_frame* frame)
{
	OBOS_UNUSED(frame);
	Arch_LAPICSendEOI();
}
OBOS_PAGEABLE_FUNCTION void Arch_LAPICInitialize(bool isBSP)
{
	if (isBSP)
	{
		uint32_t ecx = 0;
		__cpuid__(1, 0, nullptr, nullptr, &ecx, nullptr);
		has_x2apic = !!(ecx & BIT(21));
	}

	if (has_x2apic)
	{
		uintptr_t lapic_msr = rdmsr(IA32_APIC_BASE);
		lapic_msr |= APIC_ENABLE;
		lapic_msr |= BIT(10);
		if (isBSP)
		{
			lapic_msr |= APIC_BSP;
			Arch_RawRegisterInterrupt(0xfe, (uintptr_t)LAPIC_DefaultIrqHandler);
		}
		wrmsr(IA32_APIC_BASE, lapic_msr);
	
		wrmsr(IA32_X2APIC_REGISTERS + 0x0F, 0x1ff);
		wrmsr(IA32_X2APIC_REGISTERS + 0x35, (isBSP ? 0x700:0) | 0xfe);
		wrmsr(IA32_X2APIC_REGISTERS + 0x36, isBSP ? 0x400 /* NMI, Unmasked */ : 0xfe);
		// wrmsr(IA32_X2APIC_REGISTERS + 0x2f, 0x200FE);
		wrmsr(IA32_X2APIC_REGISTERS + 0x34, 0xfe);
		wrmsr(IA32_X2APIC_REGISTERS + 0x33, 0xfe);
		wrmsr(IA32_X2APIC_REGISTERS + 0x32, 0xfe);

		return;
	}

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
	if (has_x2apic)
	{
		wrmsr(IA32_X2APIC_REGISTERS + 0xb, 0);
		return;
	}

	OBOS_ASSERT(Arch_LAPICAddress);
	if (Arch_LAPICAddress)
		Arch_LAPICAddress->eoi = 0;
}

uint8_t Arch_LAPICReadID()
{
	if (!has_x2apic)
		return Arch_LAPICAddress->lapicID;
	else
	 	return rdmsr(IA32_X2APIC_REGISTERS + 0x2);
}

void Arch_WriteLAPICOffset(uint32_t offset, uint32_t value)
{
	if (has_x2apic)
		wrmsr(offset/0x10 + IA32_X2APIC_REGISTERS, value);
	else
	 	*((uint32_t*)Arch_LAPICAddress + offset/4) = value;
}

uint32_t Arch_ReadLAPICOffset(uint32_t offset)
{
	if (has_x2apic)
		return rdmsr(offset/0x10 + IA32_X2APIC_REGISTERS);
	else
	 	return *((uint32_t*)Arch_LAPICAddress + offset/4);
}

OBOS_NO_KASAN OBOS_NO_UBSAN obos_status Arch_LAPICSendIPI(ipi_lapic_info lapic, ipi_vector_info vector)
{
	if (!Arch_LAPICAddress && !has_x2apic)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	
	uint64_t icr = 0x4000 /* Level: Assert */;
	if (lapic.isShorthand)
	{
		uint64_t shorthand = lapic.info.shorthand;
		if (shorthand & ~LAPIC_DESTINATION_SHORTHAND_MASK)
			return OBOS_STATUS_INVALID_ARGUMENT;
		icr |= (shorthand << 18);
	}
	else
		icr |= ((uint64_t)lapic.info.lapicId << (has_x2apic ? 32 : 56));
	if (vector.deliveryMode == LAPIC_DELIVERY_MODE_FIXED)
		icr |= vector.info.vector;
	else if (vector.deliveryMode == LAPIC_DELIVERY_MODE_SIPI)
		icr |= (vector.info.address >> 12);
	icr |= ((uint64_t)vector.deliveryMode << 8);

	if (!has_x2apic)
	{
		Arch_LAPICAddress->interruptCommand32_63 = icr >> 32;
		Arch_LAPICAddress->interruptCommand0_31 = icr & UINT32_MAX;
	}
	else
		wrmsr(IA32_X2APIC_REGISTERS + 0x30, icr);

	if (!has_x2apic)
	{
		while (Arch_LAPICAddress->interruptCommand0_31 & (1<<12))
			pause();
	}
	else {
		do {
			icr = rdmsr(IA32_X2APIC_REGISTERS+0x30);
			pause();
		} while (icr & BIT(12));
	}
	return OBOS_STATUS_SUCCESS;
}

void CoreS_DeferIRQ(interrupt_frame* frame)
{
	Arch_LAPICSendIPI((ipi_lapic_info){.isShorthand=true,.info.shorthand=LAPIC_DESTINATION_SHORTHAND_SELF},
					  (ipi_vector_info){.deliveryMode=LAPIC_DELIVERY_MODE_FIXED,.info={.vector=frame->intNumber}});
}

void Arch_LAPICSetTimerConfiguration(uint32_t lvtTimer, uint32_t counter, uint32_t divideConfig)
{
	if (has_x2apic)
	{
		wrmsr(IA32_X2APIC_REGISTERS + 0x32, lvtTimer);
		wrmsr(IA32_X2APIC_REGISTERS + 0x38, counter);
		wrmsr(IA32_X2APIC_REGISTERS + 0x3e, divideConfig);
		return;
	}
	Arch_LAPICAddress->lvtTimer = lvtTimer;
	Arch_LAPICAddress->initialCount = counter;
	Arch_LAPICAddress->divideConfig = divideConfig;
}