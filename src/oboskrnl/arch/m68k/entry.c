/*
 * oboskrnl/arch/m68k/entry.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <text.h>
#include <klog.h>
#include <font.h>
#include <memmanip.h>

#include <arch/m68k/loader/Limine.h>

#include <allocators/base.h>

#include <scheduler/process.h>

#include <scheduler/thread.h>
#include <scheduler/cpu_local.h>

#include <arch/m68k/cpu_local_arch.h>

#include <irq/irql.h>
#include <irq/timer.h>

allocator_info* OBOS_KernelAllocator;
process *OBOS_KernelProcess;
timer_frequency CoreS_TimerFrequency;

struct limine_hhdm_request Arch_HHDMRequest = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};
cpu_local bsp_cpu;
thread kmain_thread;
thread idle_thread;
cpu_local* CoreS_GetCPULocalPtr()
{
    return &bsp_cpu;
}
void Arch_InitializeVectorTable();

// Makeshift frame buffer lol.
// BPP=32
// Width=1024P
// Height=768P
// Pitch=4096B
// Format=XRGB8888
char Arch_Framebuffer[1024*768*4];
OBOS_NO_KASAN OBOS_NO_UBSAN void Arch_KernelEntryBootstrap()
{
    for (uint16_t irq = 0; irq <= 255; irq++)
    {
        bsp_cpu.arch_specific.irqs[irq].irql = irq / 32;
        bsp_cpu.arch_specific.irqs[irq].nDefers = 0;
        bsp_cpu.arch_specific.irqs[irq].next = bsp_cpu.arch_specific.irqs[irq].prev = nullptr;
    }
    bsp_cpu.isBSP = true;
    bsp_cpu.initialized = 0;
    bsp_cpu.id = 0;
    Core_CpuInfo = &bsp_cpu;
    Core_CpuCount = 1;
    irql oldIrql = Core_RaiseIrql(IRQL_MASKED);
    OBOS_TextRendererState.fb.base = Arch_Framebuffer;
    OBOS_TextRendererState.fb.bpp = 32;
    OBOS_TextRendererState.fb.height = 768;
    OBOS_TextRendererState.fb.width = 1024;
    OBOS_TextRendererState.fb.pitch = 1024*4;
    OBOS_TextRendererState.fb.format = OBOS_FB_FORMAT_RGBX8888;
    memzero(Arch_Framebuffer, 1024*768*4);
    OBOS_TextRendererState.font = font_bin;
    OBOS_Debug("Initializing Vector Base Register.\n");
    Arch_InitializeVectorTable();
    Core_LowerIrql(oldIrql);
    // Hang.
    while(1);
}