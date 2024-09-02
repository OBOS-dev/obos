/*
 * oboskrnl/arch/m68k/goldfish_pic.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <mm/bare_map.h>
#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/pmm.h>

#include <arch/m68k/goldfish_pic.h>
#include <arch/m68k/interrupt_frame.h>
#include <arch/m68k/pmm.h>
#include <arch/m68k/boot_info.h>

#include <locks/spinlock.h>

#include <allocators/base.h>

#include <scheduler/cpu_local.h>

obos_status Arch_MapPage(page_table pt_root, uintptr_t virt, uintptr_t to, uintptr_t ptFlags);
pic* Arch_PICBases;
size_t Arch_PICCount;
static void initialize()
{
    uintptr_t pt_root = 0;
    asm ("movec.l %%srp, %0" :"=r"(pt_root) :);
    // Detect all PICs.
    BootInfoTag* cur = Arch_GetBootInfoFrom(BootInfoType_GoldfishPicBase, nullptr);
    Arch_PICCount = 6;
    Arch_PICBases = OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, Arch_PICCount, sizeof(*Arch_PICBases), nullptr);
    for (size_t i = 0; i < Arch_PICCount; i++)
    {
        memzero(&Arch_PICBases[i], sizeof(Arch_PICBases[0]));
        Arch_PICBases[i].phys_base = i ? Arch_PICBases[i - 1].phys_base+0x1000 : ((BootDeviceBase*)(cur+1))->base;
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
        Arch_MapPage(MmS_GetCurrentPageTable(), virt_base, Arch_PICBases[i].phys_base, (0b11|(0b11<<5)|(1<<7)));
        Mm_FreePhysicalPages(oldPhys, 1);
        Arch_PICBases[i].base = virt_base;
    }
}
static OBOS_NO_KASAN uint32_t read_register(pic* pic, uint8_t offset)
{
    if (!Arch_PICBases)
        initialize();
    return ((volatile uint32_t*)pic->base)[offset/4];
}
static OBOS_NO_KASAN void write_register(pic* pic, uint8_t offset, uint32_t val)
{
    if (!Arch_PICBases)
        initialize();
    ((volatile uint32_t*)pic->base)[offset/4] = val;
}
#define PENDING     0x00
#define NUMBER      0x04
#define DISABLE_ALL 0x08
#define DISABLE     0x0C
#define ENABLE      0x10
void Arch_PICClearPending(pic* on)
{
    write_register(on, DISABLE_ALL, 0);
}
uint8_t Arch_PICGetPendingCount(pic* on)
{
    return read_register(on, PENDING);
}
void Arch_PICDisable(pic* on, uint32_t line)
{
    write_register(on, DISABLE, 1<<line);
}
void Arch_PICEnable(pic* on, uint32_t line)
{
    write_register(on, ENABLE, 1<<line);
}
uint32_t Arch_PICGetPending(pic* on)
{
    return read_register(on, NUMBER);
}
void Arch_PICRegisterIRQ(uint32_t line_number, uint8_t irq)
{
    if (!Arch_PICBases)
        initialize();
    uint8_t pic_index = line_number/32; // see qemu/hw/m68k/virt.c:58
    uint8_t line = line_number%32-8;
    Arch_PICBases[pic_index].irqs[line].vector = irq;
}
void Arch_PICMaskIRQ(uint32_t line_number, bool mask)
{
    if (!Arch_PICBases)
        initialize();
    uint8_t pic_index = line_number/32; // see qemu/hw/m68k/virt.c:58
    uint8_t line = line_number%32-8;
    Arch_PICBases[pic_index].irqs[line].masked = mask;
    if (!mask)
        Arch_PICEnable(&Arch_PICBases[pic_index], line);
    else
        Arch_PICDisable(&Arch_PICBases[pic_index], line);
}
extern uintptr_t Arch_IRQHandlers[256];
static void on_defer_1(void* udata)
{
    uint32_t line_number = (uint32_t)udata;
    uint8_t pic_index = line_number/32; // see qemu/hw/m68k/virt.c:58
    uint8_t line = line_number%32-8;
    Arch_PICBases[pic_index].irqs[line].masked = false;
    Arch_PICEnable(&Arch_PICBases[pic_index], line);
}
void CoreS_SendEOI(interrupt_frame* frame)
{
    OBOS_UNUSED(frame);
	for (size_t i = 0; i < Arch_PICCount; i++)
		Arch_PICClearPending(&Arch_PICBases[i]);
}
void Arch_PICHandleIRQ(interrupt_frame* frame)
{
    pic* cur = Arch_PICBases;
    for (size_t i = 0; i < Arch_PICCount && cur; i++, cur++)
    {
        uint32_t pending = Arch_PICGetPendingCount(cur);
        uint32_t line = 0;
        for(; pending; pending &= ~(1<<line))
        {
            line = __builtin_ctz(pending);
            if (!cur->irqs[line].masked)
            {
                interrupt_frame iframe = {};
                memcpy(&iframe, frame, sizeof(*frame)-sizeof(frame->format_7));
                iframe.intNumber = cur->irqs[line].vector;
                iframe.vector = cur->irqs[line].vector - 0x40;
                cur->irqs[line].masked = true;
                Arch_PICDisable(cur, line);
                ((void(*)(interrupt_frame*))Arch_IRQHandlers[cur->irqs[line].vector])(&iframe);
                if (CoreS_GetCPULocalPtr()->arch_specific.irqs[cur->irqs[line].vector].nDefers)
                {
                    uint32_t line_pic = ((8 + i * 32)) + line;
                    CoreS_GetCPULocalPtr()->arch_specific.irqs[cur->irqs[line].vector].on_defer_callback = on_defer_1;
                    CoreS_GetCPULocalPtr()->arch_specific.irqs[cur->irqs[line].vector].udata = (void*)line_pic;
                }
                else
                {
                    cur->irqs[line].masked = false;
                    Arch_PICEnable(cur, line);
                }
            }
        }
    }
}
void Arch_PICHandleSpurious(interrupt_frame* unused)
{
    OBOS_UNUSED(unused);
}