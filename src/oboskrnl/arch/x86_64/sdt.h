/*
	oboskrnl/arch/x86_64/sdt.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	struct ACPIRSDPHeader {
		char Signature[8];
		uint8_t Checksum;
		char OEMID[6];
		uint8_t Revision;
		uint32_t RsdtAddress; // Deprecated

		// Fields only valid if Revision != 0
		uint32_t Length;
		uint64_t XsdtAddress;
		uint8_t ExtendedChecksum;
		uint8_t reserved[3];
	} __attribute__((packed));
	struct ACPISDTHeader {
		char Signature[4];
		uint32_t Length;
		uint8_t Revision;
		uint8_t Checksum;
		char OEMID[6];
		char OEMTableID[8];
		uint32_t OEMRevision;
		uint32_t CreatorID;
		uint32_t CreatorRevision;
	} __attribute__((packed));
	/// <summary>
	/// Gets a table using the signature.
	/// </summary>
	/// <param name="sdt">A pointer to the rsdt/xsdt.</param>
	/// <param name="t32">Whether the tables are 32-bit (true) or not (false).</param>
	/// <param name="nEntries">The amount of entries to search.</param>
	/// <param name="signature">The signature of the table to look for.</param>
	/// <returns>The table, or nullptr on failure.</returns>
	ACPISDTHeader* GetTableWithSignature(ACPISDTHeader* sdt, bool t32, size_t nEntries, char(*signature)[4]);
	void GetSDTFromRSDP(ACPIRSDPHeader* rsdp, ACPISDTHeader** oHeader, bool *oT32, size_t *oNEntries);
}