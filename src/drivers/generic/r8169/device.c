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
    uint32_t tmp = 0;
    DrvS_ReadIOSpaceBar(dev->bar->bar, MissedPacketCount, &tmp, 4);
    OBOS_Debug("MPC: %08x\n", tmp);
}

static void write_reg64(r8169_device* dev, uint8_t off, uint64_t val)
{
    DrvS_WriteIOSpaceBar(dev->bar->bar, off+4, val>>32, 4);
    DrvS_WriteIOSpaceBar(dev->bar->bar, off, val&0xffffffff, 4);
}

static void write_or_register(r8169_device* dev, uint8_t off, uint32_t mask, uint8_t size)
{
    uint32_t tmp = 0;
    DrvS_ReadIOSpaceBar(dev->bar->bar, off, &tmp, size);
    tmp |= mask;
    DrvS_WriteIOSpaceBar(dev->bar->bar, off, tmp, size);
}
static void write_and_register(r8169_device* dev, uint8_t off, uint32_t mask, uint8_t size)
{
    uint32_t tmp = 0;
    DrvS_ReadIOSpaceBar(dev->bar->bar, off, &tmp, size);
    tmp &= mask;
    DrvS_WriteIOSpaceBar(dev->bar->bar, off, tmp, size);
}
void r8169_read_mac(r8169_device* dev)
{
    memzero(&dev->mac_readable, sizeof(dev->mac_readable));

    uint32_t tmp = 0;
    for (uint8_t i = 0; i < 6; i++)
    {
        DrvS_ReadIOSpaceBar(dev->bar->bar, MAC0+i, &tmp, 1);
        dev->mac[i] = tmp & 0xff;
    }

    snprintf(dev->mac_readable, 19, "%02x:%02x:%02x:%02x:%02x:%02x", dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3], dev->mac[4], dev->mac[5]);
    OBOS_Debug("RTL8169: %02x:%02x:%02x: MAC Address is %s\n",
               dev->dev->location.bus,dev->dev->location.slot,dev->dev->location.function,
               dev->mac_readable
    );
}

void r8169_hw_reset(r8169_device* dev)
{
    DrvS_WriteIOSpaceBar(dev->bar->bar, ChipCmd, BIT(4) /* Reset */, 1);
    uint32_t tmp = 0;
    do {
        DrvS_ReadIOSpaceBar(dev->bar->bar, ChipCmd, &tmp, 1);
    } while(tmp & BIT(4));
}

void r8169_init_rxcfg(r8169_device *dev)
{
    DrvS_WriteIOSpaceBar(dev->bar->bar, RxConfig, 0xe700, 4);
}

void r8169_set_rxcfg_mode(r8169_device *dev)
{
    uint64_t mc_filter = 0xffffffffffffffff;
    write_reg64(dev, MAR0, mc_filter);
    // AcceptBroadcast | AcceptMyPhys | AcceptMulticast
    write_or_register(dev, RxConfig, 0b1110, 4);
}

void r8169_set_txcfg(r8169_device *dev)
{
    // TODO: Auto FIFO enable?
    DrvS_WriteIOSpaceBar(dev->bar->bar, TxConfig, 0x0700|0x3000000, 4);
}

void r8169_lock_config(r8169_device* dev)
{
    DrvS_WriteIOSpaceBar(dev->bar->bar, Cfg9346, Cfg9346_Lock, 1);
}

void r8169_unlock_config(r8169_device* dev)
{
    DrvS_WriteIOSpaceBar(dev->bar->bar, Cfg9346, Cfg9346_Unlock, 1);
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

    dev->dev->resource_cmd_register->cmd_register |= 0x3; // io space + memspace
    Drv_PCISetResource(dev->dev->resource_cmd_register);

    r8169_alloc_set(dev, Rx_Set);
    r8169_alloc_set(dev, Tx_Set);

    r8169_init_rxcfg(dev);

    DrvS_WriteIOSpaceBar(dev->bar->bar, IntrStatus, 0xffffffff, 2);
    DrvS_WriteIOSpaceBar(dev->bar->bar, IntrMask, 0x0, 2);

    r8169_hw_reset(dev);
    r8169_read_mac(dev);

    r8169_unlock_config(dev);

    DrvS_WriteIOSpaceBar(dev->bar->bar, MaxTxPacketSize, TX_PACKET_SIZE, 2);

    write_or_register(dev, CPlusCmd, BIT(3) /*PCIMulRW*/, 2);

    // Linux sets a "magic register" here
    // TODO: Should we do that too?

    DrvS_WriteIOSpaceBar(dev->bar->bar, IntrMitigate, 0x0, 2);

    DrvS_WriteIOSpaceBar(dev->bar->bar, RxMaxSize, RX_PACKET_SIZE, 4);

    write_reg64(dev, RxDescAddrLow, dev->sets_phys[Rx_Set]);
    write_reg64(dev, TxDescStartAddrLow, dev->sets_phys[Tx_Set]);

    r8169_lock_config(dev);

    DrvS_WriteIOSpaceBar(dev->bar->bar, ChipCmd, 0xC, 1);
    r8169_init_rxcfg(dev);
    r8169_set_txcfg(dev);
    r8169_set_rxcfg_mode(dev);
    DrvS_WriteIOSpaceBar(dev->bar->bar, IntrMask, ENABLED_IRQS, 2);
    DrvS_WriteIOSpaceBar(dev->bar->bar, TimerInt, 0, 2);
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
    OBOS_Debug("set %d = %p\n", set, phys);

    phys = 0;

    for (size_t i = 0; i < DESCS_IN_SET; i++)
    {
        dev->sets[set][i].vlan = 0; // reserved probably, so we zero it.

        dev->sets[set][i].command = (RX_PACKET_SIZE&0x3fff) & ~0x7;
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
