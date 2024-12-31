/*
 * drivers/test_driver/main.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>

#include <stdarg.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <scheduler/thread.h>

#include <irq/timer.h>

#include "rand.h"

#define IOCTL_TESTDRIVER_FIREWORKS 1
#define IOCTL_TESTDRIVER_ALLOCATOR 2 /* TODO */
void cleanup()
{ /* Nothing to do */ }
DRV_EXPORT void TestDriver_Fireworks(uint32_t max_iterations, int spawn_min, int spawn_max, bool stress_test);
obos_status ioctl_var(size_t nParameters, uint64_t request, va_list list)
{
    switch(request)
    {
    case IOCTL_TESTDRIVER_FIREWORKS:
        if (nParameters < 4)
            return OBOS_STATUS_INVALID_ARGUMENT;
        volatile uint32_t arg1 = va_arg(list, uint32_t);
        volatile int arg2 = va_arg(list, int);
        volatile int arg3 = va_arg(list, int);
        volatile bool arg4 = (bool)va_arg(list, uint32_t);
        TestDriver_Fireworks(arg1, arg2, arg3, arg4);
        return OBOS_STATUS_SUCCESS;
    case IOCTL_TESTDRIVER_ALLOCATOR:
        return OBOS_STATUS_UNIMPLEMENTED;
    }
    return OBOS_STATUS_INVALID_IOCTL;
}
obos_status ioctl(size_t nParameters, uint64_t request, ...)
{
    va_list list;
    va_start(list, request);
    obos_status status = ioctl_var(nParameters, request, list);
    va_end(list);
    return status;
}
__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) volatile driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = 0,
    .ftable.driver_cleanup_callback = cleanup,
    .ftable.ioctl = ioctl,
    .ftable.ioctl_var = ioctl_var,
    .driverName = "Test driver"
};

driver_id* this_driver;

OBOS_PAGEABLE_FUNCTION DRV_EXPORT void TestDriver_Test(driver_id* caller)
{
    OBOS_Log("Function in driver %d called from driver %d.\n", this_driver->id, caller->id);
}
extern char Drv_Base[];

OBOS_PAGEABLE_FUNCTION void OBOS_DriverEntry(driver_id* this)
{
    this_driver = this;
    OBOS_Log("%s: Hello from test driver #1. Driver base: %p. Driver id: %d.\n", __func__, this->base, this->id);
    TestDriver_Test(this);
    OBOS_Log("Exiting from main thread.\n");
    Core_ExitCurrentThread();
}

#ifdef __x86_64__
extern uint64_t random_seed_x86_64();
uintptr_t random_seed()
{
    uint64_t seed = random_seed_x86_64();
    if (!seed)
        seed = CoreS_GetNativeTimerTick();
    return seed;
}
asm (
    "\
    .intel_syntax noprefix;\
    .global random_seed_x86_64;\
    random_seed_x86_64:;\
    push rbp; mov rbp, rsp;\
    .rdrand:;\
	    mov eax, 1;\
	    xor ecx,ecx;\
	    cpuid;\
	    bt ecx, 30;\
	    jnc .rdseed;\
	    rdrand rax;\
	    jnc .rdrand;\
	    jmp .done;\
    .rdseed:;\
    	mov eax, 7;\
    	xor ecx,ecx;\
    	cpuid;\
    	bt ebx, 18;\
    	jnc .done;\
    	rdseed rax;\
    	jnc .rdseed;\
    .done:;\
    leave; ret;\
    .att_syntax prefix;\
    "
);
#endif