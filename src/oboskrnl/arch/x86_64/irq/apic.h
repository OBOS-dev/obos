/*
	oboskrnl/arch/x86_64/irq/apic.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

namespace obos
{
	struct LAPIC
	{
		alignas(0x10) const uint8_t resv1[0x20];
		alignas(0x10) uint32_t lapicID;
		alignas(0x10) uint32_t lapicVersion;
		alignas(0x10) const uint8_t resv2[0x40];
		alignas(0x10) uint32_t taskPriority;
		alignas(0x10) const uint32_t arbitrationPriority;
		alignas(0x10) const uint32_t processorPriority;
		alignas(0x10) uint32_t eoi; // write zero to send eoi.
		alignas(0x10) uint32_t remoteRead;
		alignas(0x10) uint32_t logicalDestination;
		alignas(0x10) uint32_t destinationFormat;
		alignas(0x10) uint32_t spuriousInterruptVector;
		alignas(0x10) const uint32_t inService0_31;
		alignas(0x10) const uint32_t inService32_63;
		alignas(0x10) const uint32_t inService64_95;
		alignas(0x10) const uint32_t inService96_127;
		alignas(0x10) const uint32_t inService128_159;
		alignas(0x10) const uint32_t inService160_191;
		alignas(0x10) const uint32_t inService192_223;
		alignas(0x10) const uint32_t inService224_255;
		alignas(0x10) const uint32_t triggerMode0_31;
		alignas(0x10) const uint32_t triggerMode32_63;
		alignas(0x10) const uint32_t triggerMode64_95;
		alignas(0x10) const uint32_t triggerMode96_127;
		alignas(0x10) const uint32_t triggerMode128_159;
		alignas(0x10) const uint32_t triggerMode160_191;
		alignas(0x10) const uint32_t triggerMode192_223;
		alignas(0x10) const uint32_t triggerMode224_255;
		alignas(0x10) const uint32_t interruptRequest0_31;
		alignas(0x10) const uint32_t interruptRequest32_63;
		alignas(0x10) const uint32_t interruptRequest64_95;
		alignas(0x10) const uint32_t interruptRequest96_127;
		alignas(0x10) const uint32_t interruptRequest128_159;
		alignas(0x10) const uint32_t interruptRequest160_191;
		alignas(0x10) const uint32_t interruptRequest192_223;
		alignas(0x10) const uint32_t interruptRequest224_255;
		alignas(0x10) uint32_t errorStatus;
		alignas(0x10) const uint8_t resv3[0x60];
		alignas(0x10) uint32_t lvtCMCI;
		alignas(0x10) uint32_t interruptCommand0_31;
		alignas(0x10) uint32_t interruptCommand32_63;
		alignas(0x10) uint32_t lvtTimer;
		alignas(0x10) uint32_t lvtThermalSensor;
		alignas(0x10) uint32_t lvtPerformanceMonitoringCounters;
		alignas(0x10) uint32_t lvtLINT0;
		alignas(0x10) uint32_t lvtLINT1;
		alignas(0x10) uint32_t lvtError;
		alignas(0x10) uint32_t initialCount;
		alignas(0x10) const uint32_t currentCount;
		alignas(0x10) const uint8_t resv4[0x40];
		alignas(0x10) uint32_t divideConfig;
		alignas(0x10) const uint8_t resv5[0x10];
	};
	struct IOAPIC
	{
		alignas(0x10) mutable uint8_t ioregsel;
		alignas(0x10) uint32_t iowin;
		void WriteRegister(uint8_t offset, uint32_t val);
		uint32_t ReadRegister(uint8_t offset) const;
	};
	struct IOAPIC_RedirectionEntry
	{
		uint8_t vector;
		uint8_t delMod : 3;
		bool destMode : 1;
		const bool delivStatus : 1;
		bool intPol : 1;
		const bool remoteIRR : 1;
		bool triggerMode : 1;
		bool mask : 1;
		const uint64_t padding : 39;
		union
		{
			struct
			{
				uint8_t setOfProcessors;
			} OBOS_PACK logical;
			struct
			{
				uint8_t resv1 : 4;
				uint8_t lapicId : 4;
			} OBOS_PACK physical;
		} OBOS_PACK destination;
	} OBOS_PACK;
	struct IOAPIC_Registers
	{
		struct {
			const uint32_t resv1 : 24;
			uint8_t ioapicID : 4;
			const uint8_t resv2 : 4;
		} OBOS_PACK ioapicId;
		struct
		{
			const uint8_t version;
			const uint8_t resv1;
			const uint8_t maximumRedirectionEntries;
			const uint8_t resv2;
		} OBOS_PACK ioapicVersion;
		struct
		{
			const uint32_t resv1 : 24;
			const uint8_t ioapicID : 4;
			const uint8_t resv2 : 4;
		} OBOS_PACK ioapicArbitrationID;
		uint32_t resv1[13];
		IOAPIC_RedirectionEntry redirectionEntries[];
	} OBOS_PACK;
	struct IOAPIC_IRQRedirectionEntry
	{
		uint8_t source;
		uint32_t globalSystemInterrupt;
	};
	extern LAPIC* g_localAPICAddress;
	LAPIC* GetLAPICAddress();
	void InitializeLAPIC(LAPIC* lapicAddress);
	void InitializeIOAPIC(IOAPIC* ioapicAddress);
	enum class DestinationShorthand
	{
		None,
		Self,
		All,
		All_Except_Self,
		HighestValue = All_Except_Self
	};
	enum class DeliveryMode
	{
		Fixed, Default = Fixed,
		Fixed_LowestPriority,
		SMI,
		NMI = 4,
		INIT,
		SIPI,
	};
	void LAPIC_SendEOI();
	void LAPIC_SendIPI(DestinationShorthand shorthand, DeliveryMode deliveryMode, uint8_t vector = 0, uint8_t _destination = 0);
	bool IOAPIC_MaskIRQ(uint8_t irq, bool mask = false);
	bool IOAPIC_MapIRQToVector(uint8_t irq, uint8_t vector);
}