/*
 * oboskrnl/arch/x86_64/timer.c
 *
 * Copyright (c) 2024-2026 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <error.h>
#include <signal.h>
#include <memmanip.h>

#include <irq/irq.h>
#include <irq/irql.h>
#include <irq/timer.h>

#include <mm/bare_map.h>

#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/thread.h>

#include <arch/x86_64/interrupt_frame.h>
#include <arch/x86_64/hpet_table.h>
#include <arch/x86_64/lapic.h>
#include <arch/x86_64/sdt.h>
#include <arch/x86_64/pmm.h>
#include <arch/x86_64/boot_info.h>
#include <arch/x86_64/ioapic.h>
#include <arch/x86_64/timer.h>
#include <arch/x86_64/asm_helpers.h>

#include <external/fixedptc.h>

#include <stdatomic.h>

extern uint64_t Arch_FindCounter(uint64_t hz);
atomic_size_t nCPUsWithInitializedTimer;

static irq_handler s_timer_cb;

void Arch_SchedulerIRQHandlerEntry(irq* obj, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    if (!CoreS_GetCPULocalPtr()->arch_specific.initializedSchedulerTimer)
    {
        Arch_LAPICSetTimerConfiguration(0x20000 | (Core_SchedulerIRQ->vector->id + 0x20),
                                        Arch_FindCounter(Core_SchedulerTimerFrequency),
                                        0xb);
        OBOS_Debug("Initialized timer for CPU %d.\n", CoreS_GetCPULocalPtr()->id);
        CoreS_GetCPULocalPtr()->arch_specific.initializedSchedulerTimer = true;
        nCPUsWithInitializedTimer++;
    }
    else
    {
        if (s_timer_cb && CoreS_GetCPULocalPtr() == Core_CpuInfo)
            s_timer_cb(obj, frame, userdata, oldIrql);
        if (frame->cs & 0x3)
            Arch_UserYield(Core_GetCurrentThread()->kernelStack); // switches to the kernel stack passed, then yields, then returns.
        else
            Core_Yield();
    }
}

HPET* Arch_HPETAddress;
uint64_t Arch_HPETFrequency;
timer_frequency CoreS_TimerFrequency;
uint64_t Arch_CalibrateHPET(uint64_t freq)
{
    if (!Arch_HPETFrequency)
        Arch_HPETFrequency = 1000000000000000 / Arch_HPETAddress->generalCapabilitiesAndID.counterCLKPeriod;
    Arch_HPETAddress->generalConfig &= ~(1 << 0);
    uint64_t compValue = Arch_HPETAddress->mainCounterValue + (Arch_HPETFrequency / freq);
    Arch_HPETAddress->timer0.timerConfigAndCapabilities &= ~(1 << 2);
    Arch_HPETAddress->timer0.timerConfigAndCapabilities &= ~(1 << 3);
    return compValue;
}
#define OffsetPtr(ptr, off, t) ((t*)(((uintptr_t)(ptr)) + (off)))
extern obos_status Arch_MapPage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags, bool e);
static OBOS_PAGEABLE_FUNCTION OBOS_NO_UBSAN void InitializeHPET()
{
    static basicmm_region hpet_region;
    hpet_region.mmioRange = true;
    ACPIRSDPHeader* rsdp = (ACPIRSDPHeader*)Arch_MapToHHDM(Arch_RSDPBase);
    bool tables32 = rsdp->Revision < 2;
    ACPISDTHeader* xsdt = tables32 ? (ACPISDTHeader*)(uintptr_t)rsdp->RsdtAddress : (ACPISDTHeader*)rsdp->XsdtAddress;
    xsdt = (ACPISDTHeader*)Arch_MapToHHDM((uintptr_t)xsdt);
    size_t nEntries = (xsdt->Length - sizeof(*xsdt)) / (tables32 ? 4 : 8);
    HPET_Table* hpet_table = nullptr;
    for (size_t i = 0; i < nEntries; i++)
    {
        uintptr_t phys = tables32 ? OffsetPtr(xsdt, sizeof(*xsdt), uint32_t)[i] : OffsetPtr(xsdt, sizeof(*xsdt), uint64_t)[i];
        ACPISDTHeader* header = (ACPISDTHeader*)Arch_MapToHHDM(phys);
        if (memcmp(header->Signature, "HPET", 4))
        {
            hpet_table = (HPET_Table*)header;
            break;
        }
    }
    if (!hpet_table)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "No HPET!\n");
    uintptr_t phys = hpet_table->baseAddress.address;
    Arch_HPETAddress = (HPET*)0xffffffffffffd000;
    Arch_MapPage(getCR3(), Arch_HPETAddress, phys, 0x8000000000000013, false);
    OBOSH_BasicMMAddRegion(&hpet_region, Arch_HPETAddress, 0x1000);
}
static void hpet_irq_move_callback(irq* i, irq_vector* from, irq_vector* to, void* userdata)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(from);
    HPET_Timer* timer = (HPET_Timer*)userdata;
    OBOS_ASSERT(timer);
    uint32_t gsi = (timer->timerConfigAndCapabilities >> 9) & 0b11111;
    Arch_IOAPICMapIRQToVector(gsi, to->id+0x20, false, TriggerModeLevelSensitive);
    Arch_IOAPICMaskIRQ(gsi, false);
}
OBOS_NO_KASAN OBOS_NO_UBSAN void hpet_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    // HPET_Timer* timer = (HPET_Timer*)i->irqCheckerUserdata;
    // OBOS_ASSERT(timer);
    // size_t timerIndex = ((uintptr_t)timer-(uintptr_t)Arch_HPETAddress-offsetof(HPET, timer0))/sizeof(HPET_Timer);
    // Arch_HPETAddress->generalInterruptStatus &= ~(1<<timerIndex);
    ((irq_handler)userdata)(i, frame, nullptr, oldIrql);
}

static bool s_use_invariant_tsc = false;
static uint64_t s_invariant_tsc_frequency = 0;
extern uint64_t Arch_FindTSCChangeRate(uint64_t deadline);

bool Arch_UsingInvTSCFrequency()
{ return s_use_invariant_tsc; }

OBOS_PAGEABLE_FUNCTION obos_status CoreS_InitializeTimer(irq_handler handler)
{
    static bool initialized = false;
    OBOS_ASSERT(!initialized);
    if (initialized)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    if (!handler)
        return OBOS_STATUS_INVALID_ARGUMENT;

    // Check for the Invariant TSC

    uint32_t edx = 0;
    __cpuid__(0x80000007, 0, nullptr, nullptr, nullptr, &edx);
    if (edx & BIT(8))
    {
        s_use_invariant_tsc = true;

        Core_IrqObjectFree(Core_TimerIRQ);
        Core_TimerIRQ = Core_SchedulerIRQ;
        s_timer_cb = handler;

        CoreS_TimerFrequency = Core_SchedulerTimerFrequency;

        uint32_t oeax = 0;
        __cpuid__(0, 0, &oeax, nullptr, nullptr, nullptr);
        if (oeax < 0x15)
        {
            uint32_t ebx = 0, eax = 0, ecx = 0;
            __cpuid__(0x15, 0, &eax, &ebx, &ecx, nullptr);
            if (eax == 0 || ecx == 0 || ebx == 0)
            {
                // Shitty hardware, or qemu.

                goto calibrate;
            }
            s_invariant_tsc_frequency = ecx * (ebx / eax);
            goto down;
        }
        calibrate:
        do {
            const int n = 1;
            uint64_t change_rate = 0;
    
            fixedptd tp = fixedpt_fromint(1); // us.0
            const fixedptd divisor = fixedpt_fromint(Arch_HPETFrequency); // 1000.0
            // OBOS_ASSERT(fixedpt_toint(tp) == (int64_t)1);
            // OBOS_ASSERT(fixedpt_toint(divisor) == Arch_HPETFrequency);
            tp = fixedpt_xdiv(tp, divisor);

            for (int i = 0; i < n; i++)
                change_rate += Arch_FindTSCChangeRate(Arch_HPETAddress->mainCounterValue + (fixedpt_toint(tp)+1));
       
            s_invariant_tsc_frequency = (change_rate / n) * 100000;
            // s_invariant_tsc_frequency = 2380800000UL;
        } while(0);

        down:
        (void)0;

        OBOS_Debug("Using invariant TSC. TSC Frequency is %zu Hz\n", s_invariant_tsc_frequency);

        return OBOS_STATUS_SUCCESS;
    }    

    obos_status status = Core_IrqObjectInitializeIRQL(Core_TimerIRQ, IRQL_TIMER, false, true);
    if (obos_is_error(status))
        return status;
    Core_TimerIRQ->moveCallback  = hpet_irq_move_callback;
    Core_TimerIRQ->handler = hpet_irq_handler;
    Core_TimerIRQ->handlerUserdata = handler;
    volatile HPET_Timer* timer = &Arch_HPETAddress->timer0;
    // TODO: Make this support choosing a different timer.
    if (!(timer->timerConfigAndCapabilities & (1<<4)))
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "HPET Timer does not support periodic mode.");
    if (!(timer->timerConfigAndCapabilities & (1<<5)))
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "HPET Timer is not a 64-bit timer.");
    Core_TimerIRQ->irqCheckerUserdata = (void*)timer;
    Core_TimerIRQ->irqMoveCallbackUserdata = (void*)timer;
    uint32_t irqRouting = timer->timerConfigAndCapabilities >> 32;
    if (!irqRouting)
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "HPET Timer does not support irq routing through the I/O APIC.");
    volatile uint32_t gsi = UINT32_MAX;
    do {
        uint32_t cgsi = __builtin_ctz(irqRouting);
        if (Arch_IOAPICGSIUsed(cgsi) == OBOS_STATUS_SUCCESS)
        {
            gsi = cgsi;
            break;
        }
        irqRouting &= (1<cgsi);
    } while (irqRouting);
    if (gsi == UINT32_MAX)
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "Could not find empty I/O APIC IRQ for the HPET. irqRouting=0x%08x\n", irqRouting);
    OBOS_ASSERT(gsi <= 32);
    timer->timerConfigAndCapabilities |= (1<6)|(1<<3)|((uint8_t)gsi<<9); // Edge-triggered IRQs, set GSI, Periodic timer
    CoreS_TimerFrequency = 1000;
    OBOS_Debug("HPET frequency: %ld, configured HPET frequency: %ld\n", Arch_HPETFrequency, CoreS_TimerFrequency);
    const uint64_t value = Arch_HPETFrequency/CoreS_TimerFrequency;
    timer->timerComparatorValue = Arch_HPETAddress->mainCounterValue + value;
    timer->timerComparatorValue = value;
    timer->timerConfigAndCapabilities |= (1<<1); // Enable IRQs
    Arch_IOAPICMapIRQToVector(gsi, Core_TimerIRQ->vector->id+0x20, true, TriggerModeEdgeSensitive);
    Arch_IOAPICMaskIRQ(gsi, false);
    Arch_HPETAddress->generalConfig = 0b01;
    initialized = true;
    return OBOS_STATUS_SUCCESS;
}

__attribute__((no_instrument_function)) timer_tick CoreS_GetTimerTick()
{
    static volatile uint64_t cached_divisor = 0;
    if (!cached_divisor)
        cached_divisor = CoreS_GetNativeTimerFrequency() / CoreS_TimerFrequency;
    if (obos_expect(s_use_invariant_tsc, true))
        return rdtsc() / cached_divisor;
    return Arch_HPETAddress->mainCounterValue / cached_divisor;
}

__attribute__((no_instrument_function)) timer_tick CoreS_GetNativeTimerTick()
{
    if (obos_expect(s_use_invariant_tsc, true))
        return rdtsc();
    if (obos_expect(!Arch_HPETAddress, false))
        return 0;
    return Arch_HPETAddress->mainCounterValue;
}

__attribute__((no_instrument_function)) uint64_t CoreS_GetNativeTimerFrequency()
{
    if (obos_expect(s_use_invariant_tsc, true))
        return s_invariant_tsc_frequency;
    return Arch_HPETFrequency;
}

uint64_t CoreS_TimerTickToNS(timer_tick tp)
{
    // 1000000000/freq*tp
    static uint64_t cached_rate = 0;
    if (!cached_rate)
        cached_rate = 1000000000/CoreS_TimerFrequency;
    return cached_rate * tp;
}

void Arch_InitializeSchedulerTimer()
{
    obos_status status = OBOS_STATUS_SUCCESS;
    InitializeHPET();
    Core_SchedulerIRQ = Core_IrqObjectAllocate(&status);
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize the scheduler IRQ. Status: %d.\n", status);
    status = Core_IrqObjectInitializeIRQL(Core_SchedulerIRQ, IRQL_DISPATCH, false, true);
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize the scheduler IRQ. Status: %d.\n", status);
    Core_SchedulerIRQ->handler = Arch_SchedulerIRQHandlerEntry;
    Core_SchedulerIRQ->handlerUserdata = nullptr;
    Core_SchedulerIRQ->irqChecker = nullptr;
    Core_SchedulerIRQ->irqCheckerUserdata = nullptr;
    // Hopefully this won't cause trouble.
    Core_SchedulerIRQ->choseVector = true;
    Core_SchedulerIRQ->vector->nIRQsWithChosenID = 1;
    ipi_lapic_info target = {
        .isShorthand = true,
        .info = {
            .shorthand = LAPIC_DESTINATION_SHORTHAND_ALL,
        }
    };
    ipi_vector_info vector = {
        .deliveryMode = LAPIC_DELIVERY_MODE_FIXED,
        .info.vector = Core_SchedulerIRQ->vector->id + 0x20
    };
    Core_LowerIrql(IRQL_PASSIVE);
    Arch_LAPICSendIPI(target, vector);
    while (nCPUsWithInitializedTimer != Core_CpuCount)
        pause();
    OBOS_Debug("%s: Scheduler timer is running at %d hz.\n", __func__, Core_SchedulerTimerFrequency);
}
