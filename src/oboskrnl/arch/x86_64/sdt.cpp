/*
	oboskrnl/arch/x86_64/sdt.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>

#include <arch/x86_64/sdt.h>

#include <limine/limine.h>

namespace obos
{
	volatile limine_hhdm_request hhdm_offset = {
		.id = LIMINE_HHDM_REQUEST,
		.revision = 0,
	};
	template<typename T>
	T mapToHHDM(T addr)
	{
		uintptr_t _addr = (uintptr_t)addr;
		return (T)(hhdm_offset.response->offset + _addr);
	}
	ACPISDTHeader* GetTableWithSignature(ACPISDTHeader* sdt, bool t32, size_t nEntries, char(*signature)[4])
	{
		uint64_t* tableAddresses64 = reinterpret_cast<uint64_t*>(sdt + 1);
		uint32_t* tableAddresses32 = reinterpret_cast<uint32_t*>(sdt + 1);
		for (size_t i = 0; i < nEntries; i++)
		{
			ACPISDTHeader* currentSDT = nullptr;
			if (t32)
				currentSDT = (ACPISDTHeader*)(uintptr_t)tableAddresses32[i];
			else
				currentSDT = (ACPISDTHeader*)tableAddresses64[i];
			currentSDT = mapToHHDM(currentSDT);
			if (memcmp(currentSDT->Signature, signature, 4))
				return currentSDT;
		}
		return nullptr;
	}
	void GetSDTFromRSDP(ACPIRSDPHeader* rsdp, ACPISDTHeader** oHeader, bool* oT32, size_t* oNEntries)
	{
		bool is32BitTables = false;
		ACPISDTHeader* xsdt = nullptr;
		if ((is32BitTables = (rsdp->Revision == 0)))
			xsdt = mapToHHDM((ACPISDTHeader*)(uintptr_t)rsdp->RsdtAddress);
		else
			xsdt = mapToHHDM((ACPISDTHeader*)(uintptr_t)rsdp->XsdtAddress);
		if (oHeader)
			*oHeader = xsdt;
		if (oT32)
			*oT32 = is32BitTables;
		if (oNEntries)
		{
			// The purpose of "(((uintptr_t)!is32BitTables + 1) * 4)" is to determine whether to use sizeof(uint32_t) or sizeof(uint64_t)
			*oNEntries = (xsdt->Length - sizeof(*xsdt)) / (((uintptr_t)!is32BitTables + 1) * 4);
		}
	}
}