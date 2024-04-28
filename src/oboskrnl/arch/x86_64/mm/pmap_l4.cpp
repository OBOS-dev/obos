/*
	oboskrnl/arch/x86_64/mm/pmap_l4.h

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/mm/pmap_l4.h>
#include <arch/x86_64/mm/palloc.h>

#include <arch/vmm_defines.h>

namespace obos
{
	namespace arch
	{
		uintptr_t PageMap::m_physAddrMask = 0;

		size_t GetPhysicalAddressBits()
		{
			uint32_t eax = 0, unused = 0;
			__cpuid__(0x80000008, 0, &eax, &unused, &unused, &unused);
			return eax & 0xff;
		}
		size_t GetVirtualAddressBits()
		{
			uint32_t eax = 0, unused = 0;
			__cpuid__(0x8000008, 0, &eax, &unused, &unused, &unused);
			return (eax >> 8) & 0xff;
		}

		OBOS_NO_KASAN uintptr_t PageMap::GetL4PageMapEntryAt(uintptr_t at)
		{
			uintptr_t* arr = (uintptr_t*)MapToHHDM(GetPageMap());
			return GetEntryAt(arr, at, 3);
		}
		OBOS_NO_KASAN uintptr_t PageMap::GetL3PageMapEntryAt(uintptr_t at)
		{
			uintptr_t* arr = (uintptr_t*)MapToHHDM(MaskPhysicalAddressFromEntry(GetL4PageMapEntryAt(at)));
			if (arr == MapToHHDM(0))
				return 0;
			return GetEntryAt(arr, at, 2);
		}
		OBOS_NO_KASAN uintptr_t PageMap::GetL2PageMapEntryAt(uintptr_t at)
		{
			uintptr_t* arr = (uintptr_t*)MapToHHDM(MaskPhysicalAddressFromEntry(GetL3PageMapEntryAt(at)));
			if (arr == MapToHHDM(0))
				return 0;
			return GetEntryAt(arr, at, 1);
		}
		OBOS_NO_KASAN uintptr_t PageMap::GetL1PageMapEntryAt(uintptr_t at)
		{
			if (GetL2PageMapEntryAt(at) & ((uintptr_t)1 << 7))
				return 0;
			uintptr_t* arr = (uintptr_t*)MapToHHDM(MaskPhysicalAddressFromEntry(GetL2PageMapEntryAt(at)));
			if (arr == MapToHHDM(0))
				return 0;
			return GetEntryAt(arr, at, 0);
		}

		OBOS_NO_KASAN uintptr_t* PageMap::AllocatePageMapAt(uintptr_t at, uintptr_t cpuFlags, uint8_t depth)
		{
			if (depth > 3 || depth == 0)
				return nullptr;
			if (!OBOS_IS_VIRT_ADDR_CANONICAL(at))
				return nullptr;
			if (!m_physAddrMask)
				m_physAddrMask = (((uint64_t)1 << GetPhysicalAddressBits()) - 1) << 12;
			cpuFlags &= ~m_physAddrMask;
			cpuFlags |= 1;
			// Clear the caching flags.
			cpuFlags &= ~(1<<3) & ~(1<<4) & ~(1<<7);
			// Clear the avaliable bits in the flags.
			cpuFlags &= ~0x07F0'0000'0000'0E00;
			auto GetPageMapEntryForDepth = [&](uintptr_t addr, uint8_t depth)->uintptr_t
				{
					switch (depth)
					{
					case 1:
						return GetL2PageMapEntryAt(addr);
					case 2:
						return GetL3PageMapEntryAt(addr);
					case 3:
						return GetL4PageMapEntryAt(addr);
					default:
						break;
					}
					return 0;
				};
			for (uint8_t i = 3; i > (3 - depth); i--)
			{
				uintptr_t* pageMap = (uintptr_t*)MapToHHDM((i + 1) == 4 ? GetPageMap() : MaskPhysicalAddressFromEntry(GetPageMapEntryForDepth(at, i + 1)));
				if (!pageMap[AddressToIndex(at, i)])
				{
					uintptr_t newTable = AllocatePhysicalPages(1);
					memzero(MapToHHDM(newTable), 4096);
					pageMap[AddressToIndex(at, i)] = newTable | cpuFlags;
				}
				else
				{
					uintptr_t entry = (uintptr_t)pageMap[AddressToIndex(at, i)];
					if ((entry & ((uintptr_t)1 << 63)) && !(cpuFlags & ((uintptr_t)1 << 63)))
						entry &= ~((uintptr_t)1 << 63);
					if (!(entry & ((uintptr_t)1 << 2)) && (cpuFlags & ((uintptr_t)1 << 2)))
						entry |= ((uintptr_t)1 << 2);
					if (!(entry & ((uintptr_t)1 << 1)) && (cpuFlags & ((uintptr_t)1 << 1)))
						entry |= ((uintptr_t)1 << 1);
					pageMap[AddressToIndex(at, i)] = entry;
				}
			}
			return (uintptr_t*)MapToHHDM(MaskPhysicalAddressFromEntry(GetPageMapEntryForDepth(at, (4-depth))));
		}
		OBOS_NO_KASAN bool PageMap::FreePageMapAt(uintptr_t at, uint8_t maxDepth)
		{
			if (!OBOS_IS_VIRT_ADDR_CANONICAL(at))
				return false;
			if (maxDepth > 3 || maxDepth == 0)
				return false;
			auto GetPageMapEntryForDepth = [&](uintptr_t addr, uint8_t depth)->uintptr_t
				{
					switch (depth)
					{
					case 1:
						return GetL2PageMapEntryAt(addr);
					case 2:
						return GetL3PageMapEntryAt(addr);
					case 3:
						return GetL4PageMapEntryAt(addr);
					case 4:
						return GetPageMap();
					default:
						break;
					}
					return 0;
				};
			for (uint8_t i = (4 - maxDepth); i < 4; i++)
			{
				if (!(GetPageMapEntryForDepth(at, i + 1) & 1))
					continue;
				uintptr_t* pageMap = (uintptr_t*)MapToHHDM(MaskPhysicalAddressFromEntry(GetPageMapEntryForDepth(at, i + 1)));
				uintptr_t phys = MaskPhysicalAddressFromEntry(pageMap[AddressToIndex(at, i)]);
				uintptr_t* subPageMap = (uintptr_t*)MapToHHDM(phys);
				if (memcmp(subPageMap, (int)0, 4096))
				{
					pageMap[AddressToIndex(at, i)] = 0;
					FreePhysicalPages(phys, 1);
					continue;
				}	
			}
			return true;
		}

		OBOS_NO_KASAN uintptr_t PageMap::GetEntryAt(uintptr_t* arr, uintptr_t virt, uint8_t level)
		{
			if (!OBOS_IS_VIRT_ADDR_CANONICAL(arr))
				return 0;
			if (!OBOS_IS_VIRT_ADDR_CANONICAL(&arr[AddressToIndex(virt, level)]))
				return 0;
			return arr[AddressToIndex(virt, level)];
		}
	}
}