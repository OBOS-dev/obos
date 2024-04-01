/*
	oboskrnl/main.cpp
 
	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <uacpi/uacpi.h>
#include <uacpi/sleep.h>

#include <irq/irql.h>

#ifdef __x86_64__
#include <limine/limine.h>
#endif

#define verify_status(st) \
if (st != UACPI_STATUS_OK)\
	obos::logger::panic(nullptr, "uACPI Failed! Status code: %d, error message: %s\n", st, uacpi_status_to_string(st));

namespace obos
{
#ifdef __x86_64__
	extern volatile limine_rsdp_request rsdp_request;
	extern volatile limine_hhdm_request hhdm_offset;
#endif
	void kmain()
	{
		logger::debug("In %s.\n", __func__);
		logger::log("%s: Initializing uACPI\n", __func__);
		uintptr_t rsdp = 0;
#ifdef __x86_64__
		rsdp = ((uintptr_t)rsdp_request.response->address - hhdm_offset.response->offset);
#endif
		uacpi_init_params params = {
			rsdp,
			{ UACPI_LOG_INFO, 0 }
		};
		uacpi_status st = uacpi_initialize(&params);
		verify_status(st);
	
		st = uacpi_namespace_load();
		verify_status(st);
	
		st = uacpi_namespace_initialize();
		verify_status(st);
		
		while(1);
	}
}

#if UINT32_MAX == UINTPTR_MAX
#define STACK_CHK_GUARD 0xe2dee396
#else
#define STACK_CHK_GUARD 0x1C747501613CB3
#endif
 
extern "C" uintptr_t __stack_chk_guard = STACK_CHK_GUARD;
 
extern "C" [[noreturn]] void __stack_chk_fail(void)
{
	obos::logger::panic(nullptr, "Stack corruption detected!\n");
}