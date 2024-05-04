/*
	oboskrnl/arch/x86_64/map.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>

#include <mm/bare_map.h>

#include <arch/x86_64/pmm.h>

#include <arch/x86_64/asm_helpers.h>

static size_t AddressToIndex(uintptr_t address, uint8_t level) { return (address >> (9 * level + 12)) & 0x1FF; }

uintptr_t Arch_MaskPhysicalAddressFromEntry(uintptr_t phys)
{
	return phys & 0xffffffffff000;
}
uintptr_t Arch_GetPML4Entry(uintptr_t pml4Base, uintptr_t addr)
{
	uintptr_t* arr = (uintptr_t*)Arch_MapToHHDM(Arch_MaskPhysicalAddressFromEntry(pml4Base));
	return arr[AddressToIndex(addr, 3)];
}
uintptr_t Arch_GetPML3Entry(uintptr_t pml4Base, uintptr_t addr)
{
	uintptr_t* arr = (uintptr_t*)Arch_MapToHHDM(Arch_MaskPhysicalAddressFromEntry(Arch_GetPML4Entry(pml4Base, addr)));
	return arr[AddressToIndex(addr, 2)];
}
uintptr_t Arch_GetPML2Entry(uintptr_t pml4Base, uintptr_t addr)
{
	uintptr_t* arr = (uintptr_t*)Arch_MapToHHDM(Arch_MaskPhysicalAddressFromEntry(Arch_GetPML3Entry(pml4Base, addr)));
	return arr[AddressToIndex(addr, 1)];
}
uintptr_t Arch_GetPML1Entry(uintptr_t pml4Base, uintptr_t addr)
{
	uintptr_t* arr = (uintptr_t*)Arch_MapToHHDM(Arch_MaskPhysicalAddressFromEntry(Arch_GetPML2Entry(pml4Base, addr)));
	return arr[AddressToIndex(addr, 0)];
}

static uintptr_t GetPageMapEntryForDepth(uintptr_t pml4Base, uintptr_t addr, uint8_t depth)
{
	switch (depth)
	{
	case 1:
		return Arch_GetPML2Entry(pml4Base, addr);
	case 2:
		return Arch_GetPML3Entry(pml4Base, addr);
	case 3:
		return Arch_GetPML4Entry(pml4Base, addr);
	default:
		break;
	}
	return 0;
}

uintptr_t* AllocatePageMapAt(uintptr_t pml4Base, uintptr_t at, uintptr_t cpuFlags, uint8_t depth)
{
	if (depth > 3 || depth == 0)
		return nullptr;
	cpuFlags &= ~0xfffffffff0000;
	cpuFlags |= 1;
	// Clear the caching flags.
	cpuFlags &= ~(1 << 3) & ~(1 << 4) & ~(1 << 7);
	// Clear the avaliable bits in the flags.
	cpuFlags &= ~0x07F0000000000E00;
	for (uint8_t i = 3; i > (3 - depth); i--)
	{
		uintptr_t* pageMap = (uintptr_t*)Arch_MapToHHDM((i + 1) == 4 ? pml4Base : Arch_MaskPhysicalAddressFromEntry(GetPageMapEntryForDepth(pml4Base, at, i + 1)));
		if (!pageMap[AddressToIndex(at, i)])
		{
			uintptr_t newTable = OBOSS_AllocatePhysicalPages(1,1, nullptr);
			memzero(Arch_MapToHHDM(newTable), 4096);
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
	return (uintptr_t*)Arch_MapToHHDM(Arch_MaskPhysicalAddressFromEntry(GetPageMapEntryForDepth(pml4Base, at, (4 - depth))));
}

obos_status OBOSS_MapPage_RW_XD(void* at_, uintptr_t phys)
{
	uintptr_t at = (uintptr_t)at_;
	if (phys & 0xfff || at & 0xfff)
		return OBOS_STATUS_INVALID_ARGUMENT;
	phys = Arch_MaskPhysicalAddressFromEntry(phys);
	const uintptr_t flags = 0x8000000000000003;
	uintptr_t* pm = AllocatePageMapAt(getCR3(), at, flags, 3);
	pm[AddressToIndex(at, 0)] = phys | flags;
	invlpg(at);
	return OBOS_STATUS_SUCCESS;
}