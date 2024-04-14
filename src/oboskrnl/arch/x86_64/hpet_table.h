/*
	oboskrnl/arch/x86_64/hpet_table.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/x86_64/sdt.h>

namespace obos
{
	namespace arch
	{
		struct HPET_Addr
		{
			uint8_t addressSpaceId;
			uint8_t registerBitWidth;
			uint8_t registerBitOffset;
			uint8_t resv;
			uintptr_t address;
		}  __attribute__((packed));
		struct HPET_Table
		{
			ACPISDTHeader sdtHeader;
			uint32_t eventTimerBlockID;
			HPET_Addr baseAddress;
			uint8_t hpetNumber;
			uint16_t mainCounterMinimum;
			uint8_t pageProtectionAndOEMAttrib;
		} __attribute__((packed));
		struct HPET_Timer
		{
			uint64_t timerConfigAndCapabilities;
			uint64_t timerComparatorValue;
			struct
			{
				uint32_t fsbIntVal;
				uint32_t fsbIntAddr;
			} timerFSBInterruptRoute;
			const uint64_t resv;
		};
		struct HPET
		{
			const struct {
				uint8_t revisionId;
				uint8_t numTimCap : 4;
				bool countSizeCap : 1;
				bool resv1 : 1;
				bool legRouteCap : 1;
				uint16_t vendorID;
				uint32_t counterCLKPeriod;
			} __attribute__((packed)) generalCapabilitiesAndID;
			const uint64_t resv1;
			uint64_t generalConfig;
			const uint64_t resv2;
			uint64_t generalInterruptStatus;
			const uint64_t resv3[0x19];
			uint64_t mainCounterValue;
			const uint64_t resv4;
			HPET_Timer timer0, timer1, timer2;
			// 0x160-0x400 are for timers 0-31
		};
		extern HPET* g_hpetAddress;
		extern uint64_t g_hpetFrequency;
	}
}