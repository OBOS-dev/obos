/*
 * oboskrnl/sanitizers/asan.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <allocators/basic_allocator.h>

#define OBOS_CROSSES_PAGE_BOUNDARY(base, size) (((uintptr_t)(base) & ~0xfff) == ((((uintptr_t)(base) + (size)) & ~0xfff)))

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
#endif


typedef enum
{
	InvalidType = 0,
	InvalidAccess,
	ShadowSpaceAccess,
	StackShadowSpaceAccess,
} asan_violation_type;
uint8_t asan_poison = 0xDE;
OBOS_NO_KASAN void asan_report(uintptr_t addr, size_t sz, uintptr_t ip, bool rw, asan_violation_type type, bool unused)
{
	switch (type)
	{
	case InvalidAccess:
		OBOS_Panic(OBOS_PANIC_KASAN_VIOLATION, "ASAN Violation at %p while trying to %s %lu bytes from 0x%p.\n", (void*)ip, rw ? "write" : "read", sz, (void*)addr);
		break;
	case ShadowSpaceAccess:
		OBOS_Panic(OBOS_PANIC_KASAN_VIOLATION, "ASAN Violation at %p while trying to %s %lu bytes from 0x%p (Hint: Pointer is in shadow space).\n", (void*)ip, rw ? "write" : "read", sz, (void*)addr);
		break;
	default:
		break;
	}
}
static OBOS_NO_KASAN bool isAllocated(uintptr_t base, size_t size, bool rw)
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
#endif
	return true;
}
OBOS_NO_KASAN void asan_shadow_space_access(uintptr_t at, size_t size, uintptr_t ip, bool rw, bool abort)
{
	// Verify this memory is actually poisoned and not just coincidentally set to the poison.
	// Note: This method might not report all shadow space accesses.
	// First check 16 bytes after and before the pointer, and if either are poisoned, it is safe to assume that this access is that of the shadow space.
	bool isPoisoned = false;
	bool shortCircuitedFirst = false, shortCircuitedSecond = false;
	if (OBOS_CROSSES_PAGE_BOUNDARY(at - 16, 16))
		if ((shortCircuitedFirst = !isAllocated(at - 16, 16, false)))
			goto short_circuit1;
	isPoisoned = memcmp_b((void*)(at - 16), asan_poison, 16);
	short_circuit1:
	if (!isPoisoned)
		if (OBOS_CROSSES_PAGE_BOUNDARY(at + size, 16))
			if ((shortCircuitedSecond = !isAllocated(at + size, 16, false)))
				goto short_circuit2;
	isPoisoned = memcmp_b((void*)(at + size), asan_poison, 16);
	short_circuit2:
	if (isPoisoned || (shortCircuitedFirst && shortCircuitedSecond))
		asan_report(at, size, ip, rw, ShadowSpaceAccess, abort);
}
OBOS_NO_KASAN void asan_verify(uintptr_t at, size_t size, uintptr_t ip, bool rw, bool abort)
{
	if (!isAllocated(at, size, rw))
		asan_report(at, size, ip, rw, InvalidAccess, abort);
	// Check for shadow space accesses for both the stack and the kernel heap.
	if (rw && memcmp_b((void*)at, asan_poison, size))
		asan_shadow_space_access(at, size, ip, rw, abort);
}

#if __INTELLISENSE__
#	define __builtin_return_address(n) n
#	define __builtin_extract_return_addr(a) a
#endif
#define ASAN_LOAD_NOABORT(size)\
OBOS_NO_KASAN void __asan_load##size##_noabort(uintptr_t addr)\
{\
asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), false, false);\
}
#define ASAN_LOAD_ABORT(size)\
OBOS_NO_KASAN void __asan_load##size(uintptr_t addr)\
{\
asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), false, true);\
}
#define ASAN_STORE_NOABORT(size)\
OBOS_NO_KASAN void __asan_store##size##_noabort(uintptr_t addr)\
{\
asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), true, false);\
}
#define ASAN_STORE_ABORT(size)\
OBOS_NO_KASAN void __asan_store##size(uintptr_t addr)\
{\
asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), true, true);\
}
ASAN_LOAD_ABORT(1)
ASAN_LOAD_ABORT(2)
ASAN_LOAD_ABORT(4)
ASAN_LOAD_ABORT(8)
ASAN_LOAD_ABORT(16)
ASAN_LOAD_NOABORT(1)
ASAN_LOAD_NOABORT(2)
ASAN_LOAD_NOABORT(4)
ASAN_LOAD_NOABORT(8)
ASAN_LOAD_NOABORT(16)

ASAN_STORE_ABORT(1)
ASAN_STORE_ABORT(2)
ASAN_STORE_ABORT(4)
ASAN_STORE_ABORT(8)
ASAN_STORE_ABORT(16)
ASAN_STORE_NOABORT(1)
ASAN_STORE_NOABORT(2)
ASAN_STORE_NOABORT(4)
ASAN_STORE_NOABORT(8)
ASAN_STORE_NOABORT(16)

OBOS_NO_KASAN void __asan_load_n(uintptr_t addr, size_t size)
{
	asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), false, true);
}
OBOS_NO_KASAN void __asan_store_n(uintptr_t addr, size_t size)
{
	asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), true, true); 
}
OBOS_NO_KASAN void __asan_loadN_noabort(uintptr_t addr, size_t size)
{
	asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), false, false);
}
OBOS_NO_KASAN void __asan_storeN_noabort(uintptr_t addr, size_t size)
{
	asan_verify(addr, size, (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)), true, false);
}
OBOS_NO_KASAN void __asan_after_dynamic_init() { return; /* STUB */ }
OBOS_NO_KASAN void __asan_before_dynamic_init() { return; /* STUB */ }
OBOS_NO_KASAN void __asan_handle_no_return() { return; /* STUB */ }