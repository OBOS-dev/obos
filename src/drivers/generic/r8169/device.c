/*
 * drivers/generic/r8169/device.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <driver_interface/pci.h>

#include <mm/pmm.h>
#include <mm/context.h>
#include <mm/page.h>
#include <mm/alloc.h>

#include "structs.h"

bool r8169_irq_checker(struct irq* i, void* userdata)
{
    OBOS_UNUSED(i);
    r8169_device* dev = userdata;
    uint32_t isr = 0;
    DrvS_ReadIOSpaceBar(dev->bar->bar, IntrStatus, &isr, 2);
    OBOS_Debug("hmmm isr=0x%04x\n", isr);
    return isr != 0;
}

void r8169_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(frame);
    OBOS_UNUSED(oldIrql);
    r8169_device* dev = userdata;
    uint32_t isr = 0;
    DrvS_ReadIOSpaceBar(dev->bar->bar, IntrStatus, &isr, 2);
    OBOS_Debug("got r8169 irq. int status register: %04x\n", isr);
    DrvS_WriteIOSpaceBar(dev->bar->bar, IntrStatus, isr, 2);
    DrvS_ReadIOSpaceBar(dev->bar->bar, IntrStatus, &isr, 2);
    OBOS_Debug("int status register after clear: %04x\n", isr);
}

void r8169_reset(r8169_device* dev)
{
    if (!dev->suspended)
    {
        Core_IrqObjectInitializeIRQL(&dev->irq, IRQL_R8169, true, true);
        dev->irq_res->irq->irq = &dev->irq;
        dev->irq_res->irq->masked = false;
        Drv_PCISetResource(dev->irq_res);
        dev->irq_res->irq->irq->irqChecker = r8169_irq_checker;
        dev->irq_res->irq->irq->handler = r8169_irq_handler;
        dev->irq_res->irq->irq->irqCheckerUserdata = dev;
        dev->irq_res->irq->irq->handlerUserdata = dev;
    }

    dev->dev->resource_cmd_register->cmd_register |= 5; // io space + bus master
    Drv_PCISetResource(dev->dev->resource_cmd_register);

    memzero(&dev->mac_readable, sizeof(dev->mac_readable));

    // Send Reset.
    DrvS_WriteIOSpaceBar(dev->bar->bar, ChipCmd, 0x10, 1);
    uint32_t val = 0;
    do {
        DrvS_ReadIOSpaceBar(dev->bar->bar, ChipCmd, &val, 1);
    } while(val & 0x10);

    for (uint8_t i = 0; i < 6; i++)
    {
        DrvS_ReadIOSpaceBar(dev->bar->bar, MAC0+i, &val, 1);
        dev->mac[i] = val & 0xff;
    }

    snprintf(dev->mac_readable, 19, "%02x:%02x:%02x:%02x:%02x:%02x", dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5]);
    OBOS_Debug("RTL8169: %02x:%02x:%02x: MAC Address is %s\n",
        dev->dev->location.bus,dev->dev->location.slot,dev->dev->location.function,
        dev->mac_readable
    );

    r8169_alloc_set(dev, Rx_Set);
    r8169_alloc_set(dev, Tx_Set);
    r8169_alloc_set(dev, TxH_Set);

    // Unlock registers.
    DrvS_WriteIOSpaceBar(dev->bar->bar, Cfg9346, 0xC0, 1);

    // Configure RxConfig
    DrvS_WriteIOSpaceBar(dev->bar->bar, RxConfig, 0xe70f, 4);

    // Configure TxConfig
    DrvS_WriteIOSpaceBar(dev->bar->bar, ChipCmd, 0x4, 1);
    DrvS_WriteIOSpaceBar(dev->bar->bar, TxConfig, 0x3000700, 4);

    // Configure packet sizes.
    DrvS_WriteIOSpaceBar(dev->bar->bar, RxMaxSize, RX_PACKET_SIZE, 2);
    DrvS_WriteIOSpaceBar(dev->bar->bar, MaxTxPacketSize, TX_PACKET_SIZE, 1);

    // Write the addresses of all sets to the NIC.

    DrvS_WriteIOSpaceBar(dev->bar->bar, RxDescAddrLow, dev->sets_phys[Rx_Set]&0xffffffff, 4);
    DrvS_WriteIOSpaceBar(dev->bar->bar, RxDescAddrHigh, dev->sets_phys[Rx_Set]>>32, 4);

    DrvS_WriteIOSpaceBar(dev->bar->bar, TxDescStartAddrLow, dev->sets_phys[Tx_Set]&0xffffffff, 4);
    DrvS_WriteIOSpaceBar(dev->bar->bar, TxDescStartAddrHigh, dev->sets_phys[Tx_Set]>>32, 4);

    DrvS_WriteIOSpaceBar(dev->bar->bar, TxHDescStartAddrLow, dev->sets_phys[TxH_Set]&0xffffffff, 4);
    DrvS_WriteIOSpaceBar(dev->bar->bar, TxHDescStartAddrHigh, dev->sets_phys[TxH_Set]>>32, 4);

    // Enable Tx/Rx.
    DrvS_WriteIOSpaceBar(dev->bar->bar, ChipCmd, 0xC, 1);
    // Enable all IRQs
    DrvS_WriteIOSpaceBar(dev->bar->bar, IntrMask, 0xffffffff, 1);

    // Lock registers
    DrvS_WriteIOSpaceBar(dev->bar->bar, Cfg9346, 0x0, 1);
}

static void* map_registers(uintptr_t phys, size_t size, bool uc)
{
    size_t phys_page_offset = (phys % OBOS_PAGE_SIZE);
    phys -= phys_page_offset;
    size = size + (OBOS_PAGE_SIZE - (size % OBOS_PAGE_SIZE));
    size += phys_page_offset;
    void* virt = Mm_VirtualMemoryAlloc(
        &Mm_KernelContext,
        nullptr, size,
        uc ? OBOS_PROTECTION_CACHE_DISABLE : 0, VMA_FLAGS_NON_PAGED,
        nullptr,
        nullptr);
    for (uintptr_t offset = 0; offset < size; offset += OBOS_PAGE_SIZE)
    {
        page_info page = {.virt=offset+(uintptr_t)virt};
        MmS_QueryPageInfo(Mm_KernelContext.pt, page.virt, &page, nullptr);
        do {
            struct page what = {.phys=page.phys};
            struct page* pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
            MmH_DerefPage(pg);
        } while(0);
        page.prot.uc = uc;
        page.phys = phys+offset;
        struct page* pg = MmH_AllocatePage(page.phys, false);
        pg->flags |= PHYS_PAGE_MMIO;
        MmS_SetPageMapping(Mm_KernelContext.pt, &page, phys + offset, false);
    }
    return virt+phys_page_offset;
}

void r8169_alloc_set(r8169_device* dev, uint8_t set)
{
    OBOS_ENSURE(set <= 2);
    if (dev->sets[set])
    {
        OBOS_Warning("RTL8169: Attempt to reallocate set %d denied.", set);
        return;
    }

    OBOS_STATIC_ASSERT(DESCS_IN_SET <= MAX_DESCS_IN_SET, "DESCS_IN_SET is greater than MAX_DESCS_IN_SET");

    size_t nPages = DESCS_IN_SET*sizeof(r8169_descriptor);
    if (nPages % OBOS_PAGE_SIZE)
        nPages += (OBOS_PAGE_SIZE-(nPages%OBOS_PAGE_SIZE));
    nPages /= OBOS_PAGE_SIZE;

#if OBOS_PAGE_SIZE >= 256
    const size_t alignment = 1;
#else
    size_t alignment = 256;
    if (alignment % OBOS_PAGE_SIZE)
        alignment += (OBOS_PAGE_SIZE-(alignment%OBOS_PAGE_SIZE));
    alignment /= OBOS_PAGE_SIZE;
#endif

    uintptr_t phys = Mm_AllocatePhysicalPages(nPages, alignment, nullptr);

    // TODO: Do any platforms require this to be mapped as UC?
    dev->sets[set] = map_registers(phys, nPages*OBOS_PAGE_SIZE, false);
    dev->sets_phys[set] = phys;

    phys = 0;

    for (size_t i = 0; i < DESCS_IN_SET; i++)
    {
        dev->sets[set][i].vlan = 0; // reserved probably, so we zero it.

        dev->sets[set][i].command = (RX_PACKET_SIZE&0x3fff);
        dev->sets[set][i].command |= NIC_OWN;

        nPages = RX_PACKET_SIZE;
        if (nPages % OBOS_PAGE_SIZE)
            nPages += (OBOS_PAGE_SIZE-(nPages%OBOS_PAGE_SIZE));
        nPages /= OBOS_PAGE_SIZE;

        phys = Mm_AllocatePhysicalPages(nPages, 1, nullptr);
        dev->sets[set][i].buf_low = phys & 0xffffffff;
#if UINTPTR_MAX == UINT64_MAX
        dev->sets[set][i].buf_high = phys >> 32;
#endif
    }
    dev->sets[set][DESCS_IN_SET - 1].command |= EOR;
}
