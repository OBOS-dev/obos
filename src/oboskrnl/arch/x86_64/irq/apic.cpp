/*
	oboskrnl/arch/x86_64/irq/apic.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/sdt.h>

#include <arch/x86_64/irq/apic.h>
#include <arch/x86_64/irq/idt.h>
#include <arch/x86_64/irq/interrupt_frame.h>
#include <arch/x86_64/irq/madt.h>

#include <limine/limine.h>

#define IA32_APIC_BASE 0x1b

namespace obos
{
	extern volatile limine_hhdm_request hhdm_offset;
	volatile limine_rsdp_request rsdp_request = {
		.id = LIMINE_RSDP_REQUEST,
		.revision = 1,
	};
	LAPIC* g_localAPICAddress;
	IOAPIC* g_IOAPICAddress;
	size_t g_szIoapicRedirectionEntries;
	IOAPIC_IRQRedirectionEntry g_ioapicRedirectionEntries[256];
	static bool s_initializedIOAPICRedirectionEntries;
	void IOAPIC::WriteRegister(uint8_t offset, uint32_t val)
	{
		ioregsel = offset;
		iowin = val;
	}
	uint32_t IOAPIC::ReadRegister(uint8_t offset) const
	{
		ioregsel = offset;
		return iowin;
	}

	LAPIC* GetLAPICAddress()
	{
		return (LAPIC*)(hhdm_offset.response->offset + (rdmsr(IA32_APIC_BASE) & ~0xfff));
	}
	void DefaultInterruptHandler(interrupt_frame* frame)
	{
		if (frame->intNumber != 0xff)
			LAPIC_SendEOI();
		else
			logger::debug("Spurious interrupt received!\n");
	}
	void InitializeLAPIC(LAPIC* lapicAddress)
	{
		if (!g_localAPICAddress)
			g_localAPICAddress = lapicAddress;
		wrmsr(IA32_APIC_BASE, rdmsr(IA32_APIC_BASE) | (1 << 11));
		lapicAddress->errorStatus = 0;
		lapicAddress->lvtLINT0 |= 0xf8;
		lapicAddress->lvtLINT1 |= 0xf9;
		lapicAddress->lvtError = 0xfa;
		lapicAddress->lvtCMCI = 0xfb;
		lapicAddress->lvtPerformanceMonitoringCounters = 0xfc;
		lapicAddress->lvtThermalSensor = 0xfd;
		lapicAddress->lvtTimer = 0xfe;
		lapicAddress->spuriousInterruptVector = 0xff;

		for (uint8_t i = 0xf8; i > 0; i++)
			RawRegisterInterrupt(i, (uintptr_t)DefaultInterruptHandler);
		lapicAddress->spuriousInterruptVector |= (1 << 8);
	}
	void InitializeIOAPIC(IOAPIC* ioapicAddress)
	{
		OBOS_ASSERTP(g_IOAPICAddress == nullptr, "");
		logger::debug("%s: Initializing I/O APIC at address 0x%p.\n", __func__, ioapicAddress);
		g_IOAPICAddress = ioapicAddress;
		// Write zero the IOAPIC id register to initialize it.
		ioapicAddress->WriteRegister(0, 0);
		if (!s_initializedIOAPICRedirectionEntries)
		{
			ACPISDTHeader* sdt = nullptr;
			size_t nEntries = 0;
			bool t32 = false;
			GetSDTFromRSDP((ACPIRSDPHeader*)rsdp_request.response->address, &sdt, &t32, &nEntries);
			char sign[4] = { 'A', 'P', 'I', 'C' };
			auto madt = (MADTTable*)GetTableWithSignature(sdt, t32, nEntries, &sign);
			ParseMADTForIOAPICRedirectionEntries(madt, g_ioapicRedirectionEntries, sizeof(g_ioapicRedirectionEntries) / sizeof(*g_ioapicRedirectionEntries));
			s_initializedIOAPICRedirectionEntries = true;
		}
	}
	void LAPIC_SendEOI()
	{
		g_localAPICAddress->eoi = 0;
	}
	namespace arch
	{
		void SendEOI(interrupt_frame*)
		{
			g_localAPICAddress->eoi = 0;
		}
	}
	void LAPIC_SendIPI(DestinationShorthand shorthand, DeliveryMode deliveryMode, uint8_t vector, uint8_t _destination)
	{
		if (!g_localAPICAddress)
			return;
		while ((g_localAPICAddress->interruptCommand0_31 >> 12 & 0b1)) pause();
		uint32_t icr1 = 0;
		uint32_t icr2 = 0;
		switch (shorthand)
		{
		case obos::DestinationShorthand::None:
			icr2 |= _destination << (56 - 32);
			break;
		default:
			break;
		}
		switch (deliveryMode)
		{
		case obos::DeliveryMode::SMI:
			vector = 0;
			break;
		case obos::DeliveryMode::NMI:
			vector = 0;
			break;
		case obos::DeliveryMode::INIT:
			vector = 0;
			break;
		default:
			break;
		}
		icr1 |= vector;
		icr1 |= ((uint32_t)deliveryMode & 0b111) << 8;
		icr1 |= (uint32_t)shorthand << 18;
		g_localAPICAddress->interruptCommand32_63 = icr2;
		g_localAPICAddress->interruptCommand0_31 = icr1;
		while ((g_localAPICAddress->interruptCommand0_31 >> 12 & 0b1)) pause();
	}
	static uint8_t getRedirectionEntryIndex(uint8_t irq)
	{
		for (size_t i = 0; i < g_szIoapicRedirectionEntries; i++)
			if (g_ioapicRedirectionEntries[i].source == irq)
				return g_ioapicRedirectionEntries[i].globalSystemInterrupt;
		return irq;
	}
#define GetIOAPICRegisterOffset(reg) ((uint8_t)(uintptr_t)(&((IOAPIC_Registers*)nullptr)->reg) / 4)
	bool IOAPIC_MaskIRQ(uint8_t irq, bool mask)
	{
		if (!g_IOAPICAddress)
			return false;
		uint32_t maximumRedirectionEntries = GetIOAPICRegisterOffset(ioapicVersion);
		maximumRedirectionEntries = (g_IOAPICAddress->ReadRegister(maximumRedirectionEntries) >> 16) & 0xff;
		uint8_t redirectionEntryIndex = getRedirectionEntryIndex(irq);
		if (redirectionEntryIndex > (maximumRedirectionEntries + 1))
			return false;
		uint64_t redirectionEntryOffset = GetIOAPICRegisterOffset(redirectionEntries[redirectionEntryIndex]);
		uint64_t _entry = (uint64_t)g_IOAPICAddress->ReadRegister(redirectionEntryOffset) | ((uint64_t)g_IOAPICAddress->ReadRegister(redirectionEntryOffset + 1) << 32);
		IOAPIC_RedirectionEntry* redirectionEntry = (IOAPIC_RedirectionEntry*)&_entry;
		redirectionEntry->mask = mask;
		g_IOAPICAddress->WriteRegister(redirectionEntryOffset, _entry);
		g_IOAPICAddress->WriteRegister(redirectionEntryOffset + 1, _entry >> 32);
		return true;
	}
	bool IOAPIC_MapIRQToVector(uint8_t irq, uint8_t vector, bool activeLow, triggerMode tm)
	{
		if (!g_IOAPICAddress)
			return false;
		uint32_t maximumRedirectionEntries = GetIOAPICRegisterOffset(ioapicVersion);
		maximumRedirectionEntries = (g_IOAPICAddress->ReadRegister(maximumRedirectionEntries) >> 16) & 0xff;
		uint8_t redirectionEntryIndex = getRedirectionEntryIndex(irq);
		if (redirectionEntryIndex >= maximumRedirectionEntries)
			return false;
		uint64_t redirectionEntryOffset = GetIOAPICRegisterOffset(redirectionEntries[redirectionEntryIndex]);
		uint64_t _entry = (uint64_t)g_IOAPICAddress->ReadRegister(redirectionEntryOffset) | ((uint64_t)g_IOAPICAddress->ReadRegister(redirectionEntryOffset + 1) << 32);
		IOAPIC_RedirectionEntry* redirectionEntry = (IOAPIC_RedirectionEntry*)&_entry;
		redirectionEntry->delMod = 0b000 /* Fixed */;
		redirectionEntry->destMode = false /* Physical Mode */;
		redirectionEntry->mask = false /* Unmasked */;
		redirectionEntry->intPol = activeLow;
		redirectionEntry->triggerMode = (bool)tm;
		redirectionEntry->vector = vector;
		redirectionEntry->destination.physical.lapicId = 0;
		g_IOAPICAddress->WriteRegister(redirectionEntryOffset, _entry);
		g_IOAPICAddress->WriteRegister(redirectionEntryOffset + 1, _entry >> 32);
		return true;
	}
}