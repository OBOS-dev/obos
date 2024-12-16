/*
 * drivers/x86/bochs_vbe/suspend.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>

#include <arch/x86_64/asm_helpers.h>

#include "io.h"

/*
 * We need to save:
 * xres
 * yres
 * virtual width
 * virtual height
 * bpp
 * the bank
 * x offset
 * y offset
 * the value of the enable register
*/

struct {
    uint32_t xres, yres;
    uint32_t virt_width, virt_height;
    uint32_t bpp;
    uint32_t bank;
    uint32_t xoffset, yoffset;
    uint32_t enable;
} saved_vals;

void on_suspend()
{
    saved_vals.xres = ReadRegister(INDEX_XRES);
    saved_vals.yres = ReadRegister(INDEX_YRES);
    saved_vals.virt_height = ReadRegister(INDEX_VIRT_HEIGHT);
    saved_vals.virt_width = ReadRegister(INDEX_VIRT_WIDTH);
    saved_vals.bpp = ReadRegister(INDEX_BPP);
    saved_vals.bank = ReadRegister(INDEX_BANK);
    saved_vals.xoffset = ReadRegister(INDEX_X_OFFSET);
    saved_vals.yoffset = ReadRegister(INDEX_Y_OFFSET);
    saved_vals.enable = ReadRegister(INDEX_ENABLE);
}

#define VGA_ATT_W 0x3C0
#define VGA_MIS_W 0x3C2
#define VGA_IS1_RC 0x3DA
#define VGA_MIS_COLOR 0x01

void on_wake()
{
    // for (volatile bool b = true; b; )
        // asm volatile ("" : :"r"(b) :"memory");

    // NOTE: PCI stuff is already restored by now.

    outb(VGA_MIS_W, VGA_MIS_COLOR);
    inb(VGA_IS1_RC);
    outb(VGA_ATT_W, 0);

    WriteRegister(INDEX_ENABLE, 0);
    WriteRegister(INDEX_BPP, saved_vals.bpp);
    WriteRegister(INDEX_XRES, saved_vals.xres);
    WriteRegister(INDEX_YRES, saved_vals.yres);
    WriteRegister(INDEX_BANK, saved_vals.bank);
    WriteRegister(INDEX_VIRT_WIDTH, saved_vals.virt_width);
    WriteRegister(INDEX_VIRT_HEIGHT, saved_vals.virt_height);
    WriteRegister(INDEX_X_OFFSET, saved_vals.xoffset);
    WriteRegister(INDEX_Y_OFFSET, saved_vals.yoffset);
    WriteRegister(INDEX_ENABLE, saved_vals.enable | BIT(7));

    outb(VGA_MIS_W, VGA_MIS_COLOR);
    inb(VGA_IS1_RC);
    outb(VGA_ATT_W, 0x20);
}
