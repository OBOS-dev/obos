/*
 * oboskrnl/arch/x86_64/hpet_table.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

#include <arch/x86_64/sdt.h>

typedef struct OBOS_PACK HPET_Addr
{
	uint8_t addressSpaceId;
	uint8_t registerBitWidth;
	uint8_t registerBitOffset;
	uint8_t resv;
	uintptr_t address;
} HPET_Addr;
typedef struct OBOS_PACK HPET_Table
{
	ACPISDTHeader sdtHeader;
	uint32_t eventTimerBlockID;
	HPET_Addr baseAddress;
	uint8_t hpetNumber;
	uint16_t mainCounterMinimum;
	uint8_t pageProtectionAndOEMAttrib;
} HPET_Table;
typedef struct OBOS_PACK HPET_Timer
{
	volatile uint64_t timerConfigAndCapabilities;
	volatile uint64_t timerComparatorValue;
	volatile struct
	{
		uint32_t fsbIntVal;
		uint32_t fsbIntAddr;
	} timerFSBInterruptRoute;
	const volatile uint64_t resv;
} HPET_Timer;
typedef struct OBOS_PACK HPET
{
	volatile const struct {
		uint8_t revisionId;
		uint8_t numTimCap : 4;
		bool countSizeCap : 1;
		bool resv1 : 1;
		bool legRouteCap : 1;
		uint16_t vendorID;
		uint32_t counterCLKPeriod;
	} OBOS_PACK generalCapabilitiesAndID;
	volatile const uint64_t resv1;
	volatile uint64_t generalConfig;
	volatile const uint64_t resv2;
	volatile uint64_t generalInterruptStatus;
	volatile const uint64_t resv3[0x19];
	volatile uint64_t mainCounterValue;
	volatile const uint64_t resv4;
	volatile HPET_Timer timer0, timer1, timer2;
	// 0x160-0x400 are for timers 0-31
} HPET;
extern HPET* Arch_HPETAddress;
extern uint64_t Arch_HPETFrequency;
