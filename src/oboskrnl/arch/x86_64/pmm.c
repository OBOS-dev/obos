/*
	oboskrnl/arch/x86_64/pmm.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <arch/x86_64/pmm.h>

#include <UltraProtocol/ultra_protocol.h>

extern struct ultra_memory_map_attribute* Arch_MemoryMap;
extern struct ultra_platform_info_attribute* Arch_LdrPlatformInfo;
obos_pmem_map_entry* MmS_GetFirstPMemMapEntry(uintptr_t* index)
{
	*index = 0;
	return &Arch_MemoryMap->entries[0];
}
// returns nullptr at the end of the list.
obos_pmem_map_entry* MmS_GetNextPMemMapEntry(obos_pmem_map_entry* current, uintptr_t* index)
{
	OBOS_UNUSED(current);
	size_t nEntries = (Arch_MemoryMap->header.size - sizeof(Arch_MemoryMap->header)) / sizeof(struct ultra_memory_map_entry);
	if (!nEntries)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "No memory map entries.\n");
	if ((*index) > nEntries)
		return nullptr;
	return &Arch_MemoryMap->entries[++(*index)];
}
#define MAP_TO_HHDM(addr, type) ((type*)(Arch_LdrPlatformInfo->higher_half_base + (uintptr_t)(addr)))
#define UNMAP_FROM_HHDM(addr) ((uintptr_t)(addr) - Arch_LdrPlatformInfo->higher_half_base)

OBOS_NO_KASAN OBOS_NO_UBSAN __attribute__((no_instrument_function)) void* Arch_MapToHHDM(uintptr_t phys)
{
	return MAP_TO_HHDM(phys, void);
}
OBOS_NO_KASAN OBOS_NO_UBSAN __attribute__((no_instrument_function)) uintptr_t Arch_UnmapFromHHDM(void* virt)
{
	return UNMAP_FROM_HHDM(virt);
}

__attribute__((no_instrument_function)) void* MmS_MapVirtFromPhys(uintptr_t addr)
{
	return Arch_MapToHHDM(addr);
}

__attribute__((no_instrument_function)) uintptr_t MmS_UnmapVirtFromPhys(void* virt)
{
	return Arch_UnmapFromHHDM(virt);
}
