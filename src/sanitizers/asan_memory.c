/*
 * oboskrnl/sanitizers/asan_memory.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>

#ifdef __x86_64__
#include <arch/x86_64/asm_helpers.h>
uintptr_t Arch_GetPML4Entry(uintptr_t pml4Base, uintptr_t addr);
uintptr_t Arch_GetPML3Entry(uintptr_t pml4Base, uintptr_t addr);
uintptr_t Arch_GetPML2Entry(uintptr_t pml4Base, uintptr_t addr);
uintptr_t Arch_GetPML1Entry(uintptr_t pml4Base, uintptr_t addr);
uintptr_t* Arch_AllocatePageMapAt(uintptr_t pml4Base, uintptr_t at, uintptr_t cpuFlags, uint8_t depth);
bool Arch_FreePageMapAt(uintptr_t pml4Base, uintptr_t at, uint8_t maxDepth);
obos_status Arch_MapPage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags);
obos_status Arch_MapHugePage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags);
#elif defined(__m68k__)
#include <mm/context.h>
obos_status Arch_GetPagePTE(page_table pt_root, uintptr_t virt, uint32_t* out);
#endif

OBOS_NO_KASAN bool KASAN_IsAllocated(uintptr_t base, size_t size, bool rw)
{
#ifdef __x86_64__
	base &= ~0xfff;
	size += (0x1000 - (size & 0xfff));
	uintptr_t flags = 1;
	if (rw)
		flags |= 2;
	for (uintptr_t addr = base; addr < (base + size); addr += 0x1000)
	{
		// First check if this is a huge page.
		uintptr_t entry = Arch_GetPML2Entry(getCR3(), addr);
		if (!(entry & flags))
			return false;
		if (entry & (1 << 7))
			goto check;
		entry = Arch_GetPML1Entry(getCR3(), addr);
		check:
		if (!(entry & flags))
			return false;
	}
#elif defined(__m68k__)
	obos_status Arch_GetPagePTE(page_table pt_root, uintptr_t virt, uint32_t* out);
	uintptr_t flags = 0b11|(0b1 << 7);
	if (!rw)
		flags |= (0b1 << 2);
	uintptr_t pt_root = 0;
	asm("movec.l %%srp, %0" :"=r"(pt_root) :);
	for (uintptr_t addr = base; addr < (base + size); addr += 0x1000)
	{
		uintptr_t entry = 0;
		Arch_GetPagePTE(pt_root, addr, &entry);
		if (!(entry & flags))
			return false;
	}
#else
#	error Unknown architecture!
#endif
	return true;
}
