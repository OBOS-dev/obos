/*
 * oboskrnl/arch/x86_64/irq/madt.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/x86_64/sdt.h>

typedef struct OBOS_PACK MADTTable
{
	ACPISDTHeader sdtHeader;
	uint32_t lapicAddress;
	uint32_t unwanted;
	// There are more entries.
} MADTTable;
typedef struct OBOS_PACK MADT_EntryHeader
{
	uint8_t type;
	uint8_t length;
} MADT_EntryHeader;
typedef struct OBOS_PACK MADT_EntryType0
{
	MADT_EntryHeader entryHeader;
	uint8_t processorID;
	uint8_t apicID;
	uint32_t flags;
} MADT_EntryType0;
typedef struct OBOS_PACK MADT_EntryType1
{
	MADT_EntryHeader entryHeader;
	uint8_t ioApicID;
	uint8_t resv1;
	uint32_t ioapicAddress;
	uint32_t globalSystemInterruptBase;
} MADT_EntryType1;
typedef struct OBOS_PACK MADT_EntryType2
{
	MADT_EntryHeader entryHeader;
	uint8_t busSource;
	uint8_t irqSource;
	uint32_t globalSystemInterrupt;
	uint16_t flags;
} MADT_EntryType2;
typedef struct OBOS_PACK MADT_EntryType3
{
	MADT_EntryHeader entryHeader;
	uint8_t nmiSource;
	uint8_t resv;
	uint16_t flags;
	uint32_t globalSystemInterrupt;
} MADT_EntryType3;
typedef struct OBOS_PACK MADT_EntryType4
{
	MADT_EntryHeader entryHeader;
	uint8_t processorID;
	uint16_t flags;
	uint8_t lINT;
} MADT_EntryType4;
typedef struct OBOS_PACK MADT_EntryType5
{
	MADT_EntryHeader entryHeader;
	uint8_t resv1[2];
	uintptr_t lapic_address;
} MADT_EntryType5;
typedef struct OBOS_PACK MADT_EntryType9
{
	MADT_EntryHeader entryHeader;
	uint8_t resv1[2];
	uint32_t x2APIC_ID;
	uint32_t flags;
	uint32_t acpiID;
} MADT_EntryType9;