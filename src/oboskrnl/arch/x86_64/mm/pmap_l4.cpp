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

		uintptr_t PageMap::GetL4PageMapEntryAt(uintptr_t at)
		{
			uintptr_t* arr = (uintptr_t*)MapToHHDM(GetPageMap());
			return GetEntryAt(arr, at, 3);
		}
		uintptr_t PageMap::GetL3PageMapEntryAt(uintptr_t at)
		{
			uintptr_t* arr = (uintptr_t*)MapToHHDM(MaskPhysicalAddressFromEntry(GetL4PageMapEntryAt(at)));
			return GetEntryAt(arr, at, 2);
		}
		uintptr_t PageMap::GetL2PageMapEntryAt(uintptr_t at)
		{
			uintptr_t* arr = (uintptr_t*)MapToHHDM(MaskPhysicalAddressFromEntry(GetL3PageMapEntryAt(at)));
			return GetEntryAt(arr, at, 1);
		}
		uintptr_t PageMap::GetL1PageMapEntryAt(uintptr_t at)
		{
			if (GetL2PageMapEntryAt(at) & ((uintptr_t)1 << 7))
				return 0;
			uintptr_t* arr = (uintptr_t*)MapToHHDM(MaskPhysicalAddressFromEntry(GetL2PageMapEntryAt(at)));
			return GetEntryAt(arr, at, 0);
		}

		uintptr_t* PageMap::AllocatePageMapAt(uintptr_t at, uintptr_t cpuFlags, uint8_t depth)
		{
			if (depth > 3 || depth == 0)
				return nullptr;
			if (!OBOS_IS_VIRT_ADDR_CANONICAL(at))
				return nullptr;
			if (!m_physAddrMask)
				m_physAddrMask = (((uint64_t)1 << GetPhysicalAddressBits()) - 1) << 12;
			cpuFlags &= ~m_physAddrMask;
			cpuFlags |= 1;
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
		bool PageMap::FreePageMapAt(uintptr_t at, uint8_t maxDepth)
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
					default:
						break;
					}
					return 0;
				};
			for (uint8_t i = (4 - maxDepth); i < 4; i++)
			{
				if (!MaskPhysicalAddressFromEntry(GetPageMapEntryForDepth(at, i + 1)))
					continue;
				uintptr_t* pageMap = (uintptr_t*)MapToHHDM((i + 1) == 4 ? GetPageMap() : MaskPhysicalAddressFromEntry(GetPageMapEntryForDepth(at, i + 1)));
				if (!pageMap[AddressToIndex(at, i)])
					continue;
				uintptr_t phys = MaskPhysicalAddressFromEntry(pageMap[AddressToIndex(at, i)]);
				FreePhysicalPages(phys, 1);
				pageMap[AddressToIndex(at, i)] = 0;
			}
			return true;
		}

		uintptr_t PageMap::GetEntryAt(uintptr_t* arr, uintptr_t virt, uint8_t level)
		{
			return arr[AddressToIndex(virt, level)];
		}
	}
}