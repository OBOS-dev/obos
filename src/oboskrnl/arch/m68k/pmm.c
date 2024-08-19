/*
	oboskrnl/arch/x86_64/pmm.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <arch/m68k/pmm.h>

#include <arch/m68k/loader/Limine.h>
#include <stdint.h>

extern volatile struct limine_memmap_request Arch_MemmapRequest;
extern volatile struct limine_hhdm_request Arch_HHDMRequest;
extern volatile struct limine_memmap_request Arch_MemmapRequest;
extern volatile struct limine_hhdm_request Arch_HHDMRequest;
obos_pmem_map_entry* MmS_GetFirstPMemMapEntry(uintptr_t* index)
{
	*index = 0;
	return Arch_MemmapRequest.response->entries[0];
}
// returns nullptr at the end of the list.
obos_pmem_map_entry* MmS_GetNextPMemMapEntry(obos_pmem_map_entry* current, uintptr_t* index)
{
	OBOS_UNUSED(current);
	size_t nEntries = (Arch_MemmapRequest.response->entry_count);
	if (!nEntries)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "No memory map entries.\n");
	if ((*index) > nEntries)
		return nullptr;
	return Arch_MemmapRequest.response->entries[++(*index)];
}
#define MAP_TO_HHDM(addr, type) ((type*)((uintptr_t)Arch_HHDMRequest.response->offset + (uintptr_t)(addr)))
#define UNMAP_FROM_HHDM(addr) ((uintptr_t)(addr) - (uintptr_t)Arch_HHDMRequest.response->offset)

OBOS_NO_UBSAN OBOS_NO_KASAN void* Arch_MapToHHDM(uintptr_t phys)
{
	return MAP_TO_HHDM(phys, void);
}
OBOS_NO_UBSAN OBOS_NO_KASAN uintptr_t Arch_UnmapFromHHDM(void* virt)
{
	return UNMAP_FROM_HHDM(virt);
}
void* MmS_MapVirtFromPhys(uintptr_t addr)
{
	return Arch_MapToHHDM(addr);
}
uintptr_t MmS_UnmapVirtFromPhys(void* virt)
{
	return Arch_UnmapFromHHDM(virt);
}