/*
 * oboskrnl/arch/m68k/goldfish_pic.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "arch/m68k/interrupt_frame.h"
#include <int.h>
#include <klog.h>

#include <mm/bare_map.h>
#include <mm/context.h>

#include <arch/m68k/goldfish_pic.h>
#include <arch/m68k/boot_info.h>

uintptr_t Arch_PICBase = 0;
obos_status Arch_MapPage(page_table pt_root, uintptr_t virt, uintptr_t to, uintptr_t ptFlags);
static void initialize()
{
    Arch_PICBase = *(uintptr_t*)(Arch_GetBootInfo(BootInfoType_GoldfishPicBase) + 1);
    if (!Arch_PICBase)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find a Goldfish PIC device.\n");
    static basicmm_region pic_region;
    OBOSH_BasicMMAddRegion(&pic_region, (void*)0xffffd000, 0x1000);
    pic_region.mmioRange = true;
    Arch_PICBase = 0xffffd000;
    uintptr_t pt_root = 0;
    asm ("movec.l %%srp, %0" :"=r"(pt_root) :);
    Arch_MapPage(pt_root, 0xffffd000, Arch_PICBase, 0b11|(1<<7)|(0b11 << 5));
}
static uint32_t read_register(uint8_t offset)
{
    if (!Arch_PICBase)
        initialize();
    return ((uint32_t*)Arch_PICBase)[offset/4];
}
static void write_register(uint8_t offset, uint32_t val)
{
    if (!Arch_PICBase)
        initialize();
    ((uint32_t*)Arch_PICBase)[offset/4] = val;
}
#define PENDING     0x00
#define NUMBER      0x04
#define DISABLE_ALL 0x08
#define DISABLE     0x0C
#define ENABLE      0x10
void Arch_PICClearPending()
{
    write_register(DISABLE_ALL, 0);
}
void Arch_PICDisable(uint8_t line)
{
    if (line >= 32) 
        return;
    write_register(DISABLE, 1<<line);
}
void Arch_PICEnable(uint8_t line)
{
    write_register(ENABLE, 1<<line);
}
uint8_t Arch_PICGetPendingCount()
{
    return read_register(PENDING);
}
uint32_t Arch_PICGetPending()
{
    return read_register(NUMBER);
}
static struct pic_irq {
    uint8_t vector;
    bool masked;
} table[32];
void Arch_PICRegisterIRQ(uint8_t line, uint8_t irq)
{
    table[line].vector = irq;
}
void Arch_PICMaskIRQ(uint8_t line, bool mask)
{
    table[line].masked = mask;
    if (!mask)
        Arch_PICEnable(line);
    else
        Arch_PICDisable(line);
}
extern uintptr_t Arch_IRQHandlers[256];
void Arch_PICHandleIRQ(interrupt_frame* frame)
{
    uint32_t pending = Arch_PICGetPendingCount();
    uint8_t line = 0;
    for(; pending; pending &= ~line)
    {
        line = __builtin_ctz(pending);
        if (!table[line].masked)
        {
            interrupt_frame iframe = *frame;
            iframe.intNumber = table[line].vector;
            iframe.vector = table[line].vector - 0x40;
            ((void(*)(interrupt_frame*))Arch_IRQHandlers[table[line].vector])(&iframe);
        }
    }
}
void Arch_PICHandleSpurious(interrupt_frame* unused)
{
    OBOS_UNUSED(unused);
}