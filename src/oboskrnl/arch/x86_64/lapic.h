/*
 * oboskrnl/arch/x86_64/lapic.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

typedef struct lapic
{
	OBOS_ALIGNAS(0x10) volatile const uint8_t resv1[0x20];
	OBOS_ALIGNAS(0x10) volatile uint32_t lapicID;
	OBOS_ALIGNAS(0x10) volatile uint32_t lapicVersion;
	OBOS_ALIGNAS(0x10) volatile const uint8_t resv2[0x40];
	OBOS_ALIGNAS(0x10) volatile uint32_t taskPriority;
	OBOS_ALIGNAS(0x10) volatile const uint32_t arbitrationPriority;
	OBOS_ALIGNAS(0x10) volatile const uint32_t processorPriority;
	OBOS_ALIGNAS(0x10) volatile uint32_t eoi; // write zero to send eoi.
	OBOS_ALIGNAS(0x10) volatile uint32_t remoteRead;
	OBOS_ALIGNAS(0x10) volatile uint32_t logicalDestination;
	OBOS_ALIGNAS(0x10) volatile uint32_t destinationFormat;
	OBOS_ALIGNAS(0x10) volatile uint32_t spuriousInterruptVector;
	OBOS_ALIGNAS(0x10) volatile const uint32_t inService0_31;
	OBOS_ALIGNAS(0x10) volatile const uint32_t inService32_63;
	OBOS_ALIGNAS(0x10) volatile const uint32_t inService64_95;
	OBOS_ALIGNAS(0x10) volatile const uint32_t inService96_127;
	OBOS_ALIGNAS(0x10) volatile const uint32_t inService128_159;
	OBOS_ALIGNAS(0x10) volatile const uint32_t inService160_191;
	OBOS_ALIGNAS(0x10) volatile const uint32_t inService192_223;
	OBOS_ALIGNAS(0x10) volatile const uint32_t inService224_255;
	OBOS_ALIGNAS(0x10) volatile const uint32_t triggerMode0_31;
	OBOS_ALIGNAS(0x10) volatile const uint32_t triggerMode32_63;
	OBOS_ALIGNAS(0x10) volatile const uint32_t triggerMode64_95;
	OBOS_ALIGNAS(0x10) volatile const uint32_t triggerMode96_127;
	OBOS_ALIGNAS(0x10) volatile const uint32_t triggerMode128_159;
	OBOS_ALIGNAS(0x10) volatile const uint32_t triggerMode160_191;
	OBOS_ALIGNAS(0x10) volatile const uint32_t triggerMode192_223;
	OBOS_ALIGNAS(0x10) volatile const uint32_t triggerMode224_255;
	OBOS_ALIGNAS(0x10) volatile const uint32_t interruptRequest0_31;
	OBOS_ALIGNAS(0x10) volatile const uint32_t interruptRequest32_63;
	OBOS_ALIGNAS(0x10) volatile const uint32_t interruptRequest64_95;
	OBOS_ALIGNAS(0x10) volatile const uint32_t interruptRequest96_127;
	OBOS_ALIGNAS(0x10) volatile const uint32_t interruptRequest128_159;
	OBOS_ALIGNAS(0x10) volatile const uint32_t interruptRequest160_191;
	OBOS_ALIGNAS(0x10) volatile const uint32_t interruptRequest192_223;
	OBOS_ALIGNAS(0x10) volatile const uint32_t interruptRequest224_255;
	OBOS_ALIGNAS(0x10) volatile uint32_t errorStatus;
	OBOS_ALIGNAS(0x10) volatile const uint8_t resv3[0x60];
	OBOS_ALIGNAS(0x10) volatile uint32_t lvtCMCI;
	OBOS_ALIGNAS(0x10) volatile uint32_t interruptCommand0_31;
	OBOS_ALIGNAS(0x10) volatile uint32_t interruptCommand32_63;
	OBOS_ALIGNAS(0x10) volatile uint32_t lvtTimer;
	OBOS_ALIGNAS(0x10) volatile uint32_t lvtThermalSensor;
	OBOS_ALIGNAS(0x10) volatile uint32_t lvtPerformanceMonitoringCounters;
	OBOS_ALIGNAS(0x10) volatile uint32_t lvtLINT0;
	OBOS_ALIGNAS(0x10) volatile uint32_t lvtLINT1;
	OBOS_ALIGNAS(0x10) volatile uint32_t lvtError;
	OBOS_ALIGNAS(0x10) volatile uint32_t initialCount;
	OBOS_ALIGNAS(0x10) volatile const uint32_t currentCount;
	OBOS_ALIGNAS(0x10) volatile const uint8_t resv4[0x40];
	OBOS_ALIGNAS(0x10) volatile uint32_t divideConfig;
	OBOS_ALIGNAS(0x10) volatile const uint8_t resv5[0x10];
} lapic;

extern lapic* Arch_LAPICAddress;
void Arch_LAPICInitialize(bool isBSP);
void Arch_LAPICSendEOI();
typedef enum
{
	LAPIC_DESTINATION_SHORTHAND_NONE,
	LAPIC_DESTINATION_SHORTHAND_SELF,
	LAPIC_DESTINATION_SHORTHAND_ALL,
	LAPIC_DESTINATION_SHORTHAND_ALL_BUT_SELF,
	LAPIC_DESTINATION_SHORTHAND_MASK = 0b11,
	LAPIC_DESTINATION_SHORTHAND_DEFAULT = LAPIC_DESTINATION_SHORTHAND_NONE,
} lapic_destination_shorthand;
typedef enum
{
	LAPIC_DELIVERY_MODE_FIXED = 0b000,
	LAPIC_DELIVERY_MODE_SMI   = 0b010,
	LAPIC_DELIVERY_MODE_NMI   = 0b100,
	LAPIC_DELIVERY_MODE_INIT  = 0b101,
	LAPIC_DELIVERY_MODE_SIPI  = 0b110,
} lapic_delivery_mode;
typedef struct ipi_lapic_info
{
	bool isShorthand;
	union
	{
		uint8_t lapicId;
		lapic_destination_shorthand shorthand;
	} info;
} ipi_lapic_info;
typedef struct ipi_vector_info
{
	lapic_delivery_mode deliveryMode;
	union
	{
		// Ignored for anything but fixed delivery mode.
		uint8_t vector;
		// Ignored for anything but SIPIs.
		uint16_t address; 
	} info;
} ipi_vector_info;
obos_status Arch_LAPICSendIPI(ipi_lapic_info lapic, ipi_vector_info vector);