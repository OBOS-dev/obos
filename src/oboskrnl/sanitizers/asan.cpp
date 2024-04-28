/*
	oboskrnl/sanitizers/asan.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <arch/vmm_map.h>
#include <arch/vmm_defines.h>

#include <vmm/init.h>
#include <vmm/page_descriptor.h>

#include <allocators/basic_allocator.h>
#include <allocators/allocator.h>

#if OBOS_KDBG_ENABLED && defined(__x86_64__)
#	include <arch/x86_64/kdbg/init.h>
#endif

using namespace obos;

namespace obos
{
	enum class asan_violation_type
	{
		InvalidType = 0,
		InvalidAccess,
		ShadowSpaceAccess,
	};
	OBOS_NO_KASAN void asan_report(uintptr_t addr, size_t sz, uintptr_t ip, bool rw, asan_violation_type type, bool abort = false)
	{
//#if OBOS_KDBG_ENABLED && defined(__x86_64__)
//		if (kdbg::g_initialized)
//			breakpoint();
//#endif
		switch (type)
		{
		case obos::asan_violation_type::InvalidAccess:
			logger::reportKASANViolation("ASAN Violation at %p while trying to %s %lu bytes from 0x%p.\n", (void*)ip, rw ? "write" : "read", sz, (void*)addr);
			break;
		case obos::asan_violation_type::ShadowSpaceAccess:
			logger::reportKASANViolation("ASAN Violation at %p while trying to %s %lu bytes from 0x%p (Hint: Pointer is in shadow space).\n", (void*)ip, rw ? "write" : "read", sz, (void*)addr);
			break;
		default:
			break;
		}
		/*else
			logger::error(format, (void*)ip, rw ? "write" : "read", sz, (void*)addr);*/
	}
	static OBOS_NO_KASAN bool isAllocated(uintptr_t base, size_t size)
	{
		base -= (base % OBOS_PAGE_SIZE);
		size_t ps = 0;
		size_t sz = size;
		if (sz % OBOS_PAGE_SIZE)
			sz += (OBOS_PAGE_SIZE - (sz % OBOS_PAGE_SIZE));
		vmm::page_descriptor pd{};
		for (uintptr_t addr = base; addr <= (base + sz); addr += ps)
		{
			arch::get_page_descriptor((vmm::Context*)nullptr, (void*)addr, pd);
			if (!pd.present)
				return false;
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			ps = pd.isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
#else
			ps = OBOS_PAGE_SIZE;
#endif
		}
		return true;
	}
	OBOS_NO_KASAN void asan_verify(uintptr_t at, size_t size, uintptr_t ip, bool rw, bool abort)
	{
		bool crossesPageBoundary = OBOS_CROSSES_PAGE_BOUNDARY(at, size);
		if (crossesPageBoundary)
			size += OBOS_PAGE_SIZE;
		vmm::page_descriptor pd{};
		size_t ps = 0;
		uintptr_t base = at;
		base -= (base % OBOS_PAGE_SIZE);
		size_t sz = size;
		if (sz % OBOS_PAGE_SIZE)
			sz += (OBOS_PAGE_SIZE - (sz % OBOS_PAGE_SIZE));
		for (uintptr_t addr = base; addr < (base + sz); addr += ps)
		{
			arch::get_page_descriptor((vmm::Context*)nullptr, (void*)addr, pd);
			if (!pd.present)
			{
				asan_report(addr, size, ip, rw, asan_violation_type::InvalidAccess, abort);
				return;
			}
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			ps = pd.isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
#else
			ps = OBOS_PAGE_SIZE;
#endif
		}
		// Check if the address is poisoned.
		if (memcmp((void*)at, 0xA5, size))
			asan_report(at, size, ip, rw, asan_violation_type::ShadowSpaceAccess, abort);
	}
}

extern "C"
{
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
}