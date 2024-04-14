/*
	oboskrnl/arch/x86_64/irq/madt.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <arch/x86_64/sdt.h>

#include <arch/x86_64/irq/madt.h>

namespace obos
{
	void ScanMADT(MADTTable* madt, void(*callback)(MADT_EntryHeader* header, void* userdata), void* userdata, uint8_t entryType)
	{
		MADT_EntryHeader* entryHeader = (MADT_EntryHeader*)(madt + 1);
		void* end = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(madt) + madt->sdtHeader.Length);
		for (; entryHeader < end; entryHeader = reinterpret_cast<MADT_EntryHeader*>(reinterpret_cast<uintptr_t>(entryHeader) + entryHeader->length))
			if (entryHeader->type == entryType)
				callback(entryHeader, userdata);
	}
	size_t ParseMADTForIOAPICAddresses(MADTTable* madt, uintptr_t* addresses, size_t maxEntries)
	{
		uintptr_t userdata[] = { (uintptr_t)addresses, (uintptr_t)maxEntries, 0 };
		ScanMADT(madt, [](MADT_EntryHeader* _header, void* udata) 
			{
				uintptr_t *userdata = (uintptr_t*)udata;
				MADT_EntryType1* header = (MADT_EntryType1*)_header;
				uintptr_t* addresses = (uintptr_t*)userdata[0];
				size_t maxEntries = (size_t)userdata[1];
				if (userdata[2]++ < maxEntries)
					addresses[userdata[2] - 1] = header->ioapicAddress;
			}, userdata, 1);
		if (maxEntries > userdata[2])
			maxEntries = userdata[2];
		return userdata[2] - maxEntries;
	}
	size_t ParseMADTForIOAPICRedirectionEntries(MADTTable* madt, IOAPIC_IRQRedirectionEntry* entries, size_t maxEntries)
	{
		uintptr_t userdata[] = { (uintptr_t)entries, (uintptr_t)maxEntries, 0 };
		ScanMADT(madt, [](MADT_EntryHeader* _header, void* udata)
			{
				uintptr_t* userdata = (uintptr_t*)udata;
				MADT_EntryType2* header = (MADT_EntryType2*)_header;
				IOAPIC_IRQRedirectionEntry* addresses = (IOAPIC_IRQRedirectionEntry*)userdata[0];
				size_t maxEntries = (size_t)userdata[1];
				if (userdata[2]++ < maxEntries)
				{
					addresses[userdata[2] - 1].globalSystemInterrupt = header->globalSystemInterrupt;
					addresses[userdata[2] - 1].source = header->irqSource;
				}
			}, userdata, 2);
		if (maxEntries > userdata[2])
			maxEntries = userdata[2];
		return userdata[2] - maxEntries;
	}
	size_t ParseMADTForLAPICIds(MADTTable* madt, uint8_t* lapicIds, size_t maxEntries, bool cpuEnabled, bool onlineCapable)
	{
		uintptr_t userdata[] = { (uintptr_t)lapicIds, (uintptr_t)maxEntries, 0, (((uintptr_t)cpuEnabled)<<0)|(((uintptr_t)onlineCapable)<<1) };
		ScanMADT(madt, [](MADT_EntryHeader* _header, void* udata)
			{
				uintptr_t* userdata = (uintptr_t*)udata;
				MADT_EntryType0* header = (MADT_EntryType0*)_header;
				uint8_t* addresses = (uint8_t*)userdata[0];
				size_t maxEntries = (size_t)userdata[1];
				uintptr_t requiredFlags = userdata[2];
				if ((header->flags & requiredFlags) == requiredFlags)
					if (userdata[2]++ < maxEntries)
						addresses[userdata[2] - 1] = header->apicID;
			}, userdata, 0);
		if (maxEntries > userdata[2])
			maxEntries = userdata[2];
		return userdata[2] - maxEntries;
	}
}