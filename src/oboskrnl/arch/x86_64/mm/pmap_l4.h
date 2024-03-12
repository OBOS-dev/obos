/*
	oboskrnl/arch/x86_64/mm/pmap_l4.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace arch
	{
		size_t GetPhysicalAddressBits();
		size_t GetVirtualAddressBits();

		class PageMap
		{
		public:
			PageMap() = delete;

			// L4 -> Page Map
			// L3 -> Page Directory Pointer
			// L2 -> Page Directory
			// L1 -> Page Table
			// L0 -> Page Table Entry

			uintptr_t GetPageMap() { return (uintptr_t)this; }
			uintptr_t GetL4PageMapEntryAt(uintptr_t at); // pageMap[addressToIndex(at, 3)];
			uintptr_t GetL3PageMapEntryAt(uintptr_t at); // getL4PageMapEntryAt()[addressToIndex(at,2)];
			uintptr_t GetL2PageMapEntryAt(uintptr_t at); // getL3PageMapEntryAt()[addressToIndex(at,1)];
			uintptr_t GetL1PageMapEntryAt(uintptr_t at); // getL2PageMapEntryAt()[addressToIndex(at,0)];

			uintptr_t* AllocatePageMapAt(uintptr_t at, uintptr_t cpuFlags, uint8_t depth = 3);
			bool FreePageMapAt(uintptr_t at, uint8_t maxDepth = 3);

			static uintptr_t MaskPhysicalAddressFromEntry(uintptr_t entry)
			{
				if (!m_physAddrMask)
					m_physAddrMask = (((uint64_t)1 << GetPhysicalAddressBits()) - 1) << 12;
				return entry & m_physAddrMask;
			}
			static size_t AddressToIndex(uintptr_t address, uint8_t level) { return (address >> (9 * level + 12)) & 0x1FF; }
		private:
			static uintptr_t m_physAddrMask;
			static uintptr_t GetEntryAt(uintptr_t* arr, uintptr_t virt, uint8_t level);
		};
	}
}