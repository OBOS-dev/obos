/*
	oboskrnl/arch/x86_64/irq/madt.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/x86_64/sdt.h>

#include <arch/x86_64/irq/apic.h>

namespace obos
{
	struct MADTTable
	{
		ACPISDTHeader sdtHeader;
		uint32_t lapicAddress;
		uint32_t unwanted;
		// There are more entries.
	} OBOS_PACK;
	struct MADT_EntryHeader
	{
		uint8_t type;
		uint8_t length;
	} OBOS_PACK;
	struct MADT_EntryType0
	{
		MADT_EntryHeader entryHeader;
		uint8_t processorID;
		uint8_t apicID;
		uint32_t flags;
	} OBOS_PACK;
	struct MADT_EntryType1
	{
		MADT_EntryHeader entryHeader;
		uint8_t ioApicID;
		uint8_t resv1;
		uint32_t ioapicAddress;
		uint32_t globalSystemInterruptBase;
	} OBOS_PACK;
	struct MADT_EntryType2
	{
		MADT_EntryHeader entryHeader;
		uint8_t busSource;
		uint8_t irqSource;
		uint32_t globalSystemInterrupt;
		uint16_t flags;
	} OBOS_PACK;
	struct MADT_EntryType3
	{
		MADT_EntryHeader entryHeader;
		uint8_t nmiSource;
		uint8_t resv;
		uint16_t flags;
		uint32_t globalSystemInterrupt;
	} OBOS_PACK;
	struct MADT_EntryType4
	{
		MADT_EntryHeader entryHeader;
		uint8_t processorID;
		uint16_t flags;
		uint8_t lINT;
	} OBOS_PACK;
	struct MADT_EntryType5
	{
		MADT_EntryHeader entryHeader;
		uint8_t resv1[2];
		uintptr_t lapic_address;
	} OBOS_PACK;
	struct MADT_EntryType9
	{
		MADT_EntryHeader entryHeader;
		uint8_t resv1[2];
		uint32_t x2APIC_ID;
		uint32_t flags;
		uint32_t acpiID;
	} OBOS_PACK;
	/// <summary>
	/// Parses the MADT for I/O APIC Addresses.
	/// </summary>
	/// <param name="table">The MADT table.</param>
	/// <param name="addresses">[out] The addresses of the I/O APICs on the computer.</param>
	/// <param name="maxEntries">The max entries that the 'addresses' parameter can hold.</param>
	/// <returns>How many entries are left to store.</returns>
	size_t ParseMADTForIOAPICAddresses(MADTTable* table, uintptr_t* addresses, size_t maxEntries);
	/// <summary>
	/// Parses the MADT for I/O APIC Redirection Entries.
	/// </summary>
	/// <param name="table">The MADT table.</param>
	/// <param name="entries">[out] The redirection entries.</param>
	/// <param name="maxEntries">The maximum entries that the 'entries' parameter can hold.</param>
	/// <returns>How many entries are left to store.</returns>
	size_t ParseMADTForIOAPICRedirectionEntries(MADTTable* table, IOAPIC_IRQRedirectionEntry* entries, size_t maxEntries);
	/// <summary>
	/// Parses the MADT for LAPIC ids.
	/// </summary>
	/// <param name="table">The MADT table.</param>
	/// <param name="lapicIds">[out] The lapic ids.</param>
	/// <param name="maxEntries">The maximum entries that the 'lapicIds' parameter can hold.</param>
	/// <returns>How many entries are left to store.</returns>
	size_t ParseMADTForLAPICIds(MADTTable* table, uint8_t* lapicIds, size_t maxEntries);
}