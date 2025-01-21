/*
 * oboskrnl/arch/m68k/goldfish_rtc.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <arch/m68k/interrupt_frame.h>
#include <arch/m68k/goldfish_pic.h>
#include <arch/m68k/boot_info.h>

#include <irq/irq.h>
#include <irq/irql.h>
#include <irq/timer.h>

#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/bare_map.h>
#include <mm/pmm.h>

obos_status Arch_MapPage(page_table pt_root, uintptr_t virt, uintptr_t to, uintptr_t ptFlags);

// Because of qemu weirdness, this can be used as a timer.
BootDeviceBase Arch_RTCBase;
// typedef struct gf_rtc
// {
//     const uint32_t time_low;  // read first
//     const uint32_t time_high; // read second
//     uint32_t alarm_low;  // write second to arm
//     uint32_t alarm_high; // write first
//     uint32_t enable_irq;
//     uint32_t clear_alarm;
//     const uint32_t alarm_status;
//     uint32_t clear_irq;
// } OBOS_PACK gf_rtc;
#define TIME_LOW     0x00 /* The low dword of the time register */
#define TIME_HIGH    0x04 /* The high dword of the time register */
#define ALARM_LOW    0x08 /* The low dword of the alarm register */
#define ALARM_HIGH   0x0C /* The high dword of the alarm register */
#define ENABLE_IRQ   0x10 /* Write one to enable IRQs, or zero to disable IRQs. */
#define CLEAR_ALARM  0x14 /* Clears the alarm register */
#define ALARM_STATUS 0x18 /* On read, returns whether the alarm is running (non zero) or not (zero)*/
#define CLEAR_IRQ    0x1c /* Clears pending IRQs on write */
typedef uint32_t gf_rtc;
static const uint64_t ns_period = 4000000;
static OBOS_NO_KASAN uint32_t read_register32(volatile gf_rtc* rtc, uint8_t reg)
{
    return rtc[reg/4];
}
static OBOS_NO_KASAN uint64_t read_register64(volatile gf_rtc* rtc, uint8_t reg)
{
    uint32_t half1 = (uint32_t)(read_register32(rtc, reg));
    uint32_t half2 = (uint32_t)(read_register32(rtc, reg+4));
    return (uint64_t)half1|((uint64_t)half2 << 32);
}
static OBOS_NO_KASAN void write_register32(volatile gf_rtc* rtc, uint8_t reg, uint32_t val)
{
    if (reg <= TIME_HIGH)
        return; // Drop the read
    rtc[reg/4] = val;
}

static OBOS_NO_KASAN void write_register64(volatile gf_rtc* rtc, uint8_t reg, uint64_t val)
{
    write_register32(rtc, reg+4, val>>32);
    write_register32(rtc, reg, val);
}

static void set_alarm(volatile gf_rtc* rtc)
{
    write_register64(rtc, ALARM_LOW, read_register64(rtc, TIME_LOW) + ns_period);
    write_register32(rtc, ENABLE_IRQ, 1);
} 

timer_tick CoreS_GetTimerTick()
{
    volatile gf_rtc* rtc = (volatile gf_rtc*)(Arch_RTCBase.base);
    return read_register64(rtc,TIME_LOW);
}

uint64_t CoreS_GetNativeTimerFrequency()
{
    return CoreS_TimerFrequency;
}
timer_tick CoreS_GetNativeTimerTick()
{
    return CoreS_GetTimerTick();
}

void rtc_irq_move_callback(struct irq* i, struct irq_vector* from, struct irq_vector* to, void* userdata)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(from);
    OBOS_UNUSED(to);
    OBOS_UNUSED(userdata);
}

void rtc_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(frame);
    OBOS_UNUSED(userdata);
    OBOS_UNUSED(oldIrql);
    write_register32((volatile gf_rtc*)Arch_RTCBase.base, CLEAR_IRQ, 1);
    ((irq_handler)userdata)(i, frame, nullptr, oldIrql);
    // set_alarm((volatile gf_rtc*)(Arch_RTCBase.base));
}

// TODO: Implement
// bool rtc_check_irq_callback(struct irq* i, void* userdata)
// {
//     OBOS_UNUSED(i);
//     OBOS_UNUSED(userdata);
//     return ((volatile gf_rtc*)(Arch_RTCBase.base))->alarm_status;
// }
OBOS_PAGEABLE_FUNCTION obos_status CoreS_InitializeTimer(irq_handler handler)
{
    if (Arch_RTCBase.base)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    if (!handler)
		return OBOS_STATUS_INVALID_ARGUMENT;
    obos_status status = Core_IrqObjectInitializeIRQL(Core_TimerIRQ, IRQL_TIMER, false, false);
	if (obos_is_error(status))
		return status;
	Core_TimerIRQ->moveCallback  = rtc_irq_move_callback;
	// Core_TimerIRQ->irqChecker  = rtc_check_irq_callback;
	Core_TimerIRQ->handler = rtc_irq_handler;
	Core_TimerIRQ->handlerUserdata = handler;
    Arch_RTCBase = *(BootDeviceBase*)(Arch_GetBootInfo(BootInfoType_GoldfishRtcBase) + 1);
    // Map it.
    uintptr_t virt_base =
        (uintptr_t)Mm_VirtualMemoryAlloc(
            &Mm_KernelContext, 
            nullptr,
            0x1000,
            OBOS_PROTECTION_CACHE_DISABLE,
            VMA_FLAGS_NON_PAGED, 
            nullptr, nullptr);
    uintptr_t oldPhys = 0;
    MmS_QueryPageInfo(MmS_GetCurrentPageTable(), virt_base, nullptr, &oldPhys);
    // Map as RW, Cache Disabled, and Supervisor
    Arch_MapPage(MmS_GetCurrentPageTable(), virt_base, Arch_RTCBase.base, (0b11|(0b11<<5)|(1<<7)));
    Mm_FreePhysicalPages(oldPhys, 1);
    Arch_RTCBase.base = virt_base;
    irql oldIrql = Core_RaiseIrql(IRQL_TIMER);
    volatile gf_rtc* rtc = (volatile gf_rtc*)(Arch_RTCBase.base);
    Arch_PICMaskIRQ(Arch_RTCBase.irq, true);
    Arch_PICRegisterIRQ(Arch_RTCBase.irq, Core_TimerIRQ->vector->id + 0x40);
    // Reset the alarm by clearing it and clearing the irq status.
    write_register32(rtc, CLEAR_ALARM, 1);
    write_register32(rtc, CLEAR_IRQ, 1);
    // Enable the alarm IRQ.
    CoreS_TimerFrequency = 250;
    write_register64(rtc, ALARM_LOW, read_register64(rtc, TIME_LOW) + ns_period);
    write_register32(rtc, ENABLE_IRQ, 1);
    Arch_PICMaskIRQ(Arch_RTCBase.irq, false);
    Core_LowerIrql(oldIrql);
    return OBOS_STATUS_SUCCESS;
}

obos_status CoreS_ResetTimer()
{
    if (!Arch_RTCBase.base)
        return OBOS_STATUS_UNINITIALIZED;
    set_alarm((volatile gf_rtc*)(Arch_RTCBase.base));
    return OBOS_STATUS_SUCCESS;
}
