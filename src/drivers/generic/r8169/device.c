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

#include <irq/dpc.h>
#include <irq/irq.h>

#include <locks/pushlock.h>
#include <locks/event.h>
#include <locks/wait.h>
#include <locks/mutex.h>

#include <allocators/base.h>

#include <utils/list.h>

#include <power/shutdown.h>

#include "structs.h"

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

static void dpc_handler(dpc* obj, void* userdata);
static void* map_registers(uintptr_t phys, size_t size, bool uc, bool mmio);

bool r8169_irq_checker(struct irq* i, void* userdata)
{
    OBOS_UNUSED(i);
    r8169_device* dev = userdata;
    uint32_t isr = 0;
    DrvS_ReadIOSpaceBar(dev->bar->bar, IntrStatus, &isr, 2);
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
    dev->isr = isr;
    DrvS_WriteIOSpaceBar(dev->bar->bar, IntrStatus, isr, 2);
    DrvS_ReadIOSpaceBar(dev->bar->bar, IntrStatus, &isr, 2);

    CoreH_InitializeDPC(&dev->dpc, dpc_handler, Core_DefaultThreadAffinity);
}

void r8169_rx(r8169_device* dev)
{
    Core_PushlockAcquire(&dev->rx_buffer_lock, false);
    for (size_t i = 0; i < DESCS_IN_SET; i++)
    {
        r8169_descriptor* desc = &dev->sets[Rx_Set][i];
        if (desc->command & NIC_OWN)
            continue;

        dev->rx_count++;

        if (desc->command & RES_ERR)
        {
            // Uh oh
            OBOS_Debug("packet error!\n");
            dev->rx_errors++;
            if (desc->command & CRC_ERR)
                dev->rx_crc_errors++;
            if (desc->command & (RUNT_ERR|RWT_ERR))
                dev->rx_length_errors++;
            goto done;
        }

        // TODO: Support fragmented packets.
        if ((desc->command & (LS|FS)) != (LS|FS))
        {
            dev->rx_dropped++;
            dev->rx_length_errors++;
            goto done;
        }

        size_t packet_len = desc->command & PACKET_LEN_MASK;

        r8169_frame frame = {};
        void* buff = map_registers(desc->buf, packet_len, false, true);
        r8169_frame_generate(dev, &frame, buff, packet_len, FRAME_PURPOSE_RX /* We're receiving a packet */);
        r8169_buffer_add_frame(&dev->rx_buffer, &frame);
        if (memcmp(buff, "OBOS_Shutdown", 13))
            OBOS_Shutdown();
        Mm_VirtualMemoryFree(&Mm_KernelContext, buff, packet_len);

        dev->rx_bytes += packet_len;
        Core_EventPulse(&dev->rx_buffer.envt, false);

        done:
        r8169_release_desc(dev, desc, Rx_Set);
    }
    Core_PushlockRelease(&dev->rx_buffer_lock, false);
}

static void tx_set(r8169_device* dev, uint8_t set)
{
    if (!dev->sets[set])
        return;
    Core_PushlockAcquire(&dev->tx_buffer_lock, true);
    for (size_t i = 0; i < DESCS_IN_SET; i++)
    {
        r8169_descriptor* desc = &dev->sets[set][i];
        if (desc->command & NIC_OWN)
            continue;

        dev->tx_count++;

        Mm_FreePhysicalPages(desc->buf, TX_PACKET_SIZE*128);

        Core_EventPulse(&dev->tx_buffer.envt, false);

        r8169_release_desc(dev, desc, set);
    }
    uint32_t tx_poll = 0;
    DrvS_ReadIOSpaceBar(dev->bar->bar, TxPoll, &tx_poll, 1);
    if (set == TxH_Set && ~tx_poll & BIT(7))
    {
        dev->tx_bytes += dev->tx_high_priority_awaiting_transfer;
        dev->tx_high_priority_awaiting_transfer = 0;
    }
    if (set == Tx_Set && ~tx_poll & BIT(6))
    {
        dev->tx_bytes += dev->tx_awaiting_transfer;
        dev->tx_awaiting_transfer = 0;
    }
    Core_PushlockRelease(&dev->tx_buffer_lock, true);
}
void r8169_tx(r8169_device* dev)
{
    tx_set(dev, Tx_Set);
    tx_set(dev, TxH_Set);
}

static void dpc_handler(dpc* obj, void* userdata)
{
    OBOS_UNUSED(obj);

    r8169_device* dev = userdata;
    r8169_rx(dev);
    r8169_tx(dev);
}

void r8169_set_irq_mask(r8169_device* dev, uint16_t mask)
{
    DrvS_WriteIOSpaceBar(dev->bar->bar, IntrMask, mask, 2);
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

static uint16_t read_phy(r8169_device* dev, uint8_t offset)
{
    offset &= ~0x1f;
    uint32_t phyar_value = BIT(31);
    phyar_value |= (offset<<16);
    DrvS_WriteIOSpaceBar(dev->bar->bar, PhyAr, phyar_value, 4);
    do {
        DrvS_ReadIOSpaceBar(dev->bar->bar, PhyAr, &phyar_value, 4);
    } while(phyar_value & BIT(31));
    return phyar_value & 0xffff;
}

static void write_phy(r8169_device* dev, uint8_t offset, uint16_t data)
{
    offset &= ~0x1f;
    uint32_t phyar_value = 0;
    phyar_value |= (offset<<16);
    phyar_value |= (data&0xffff);
    DrvS_WriteIOSpaceBar(dev->bar->bar, PhyAr, phyar_value, 4);
    do {
        DrvS_ReadIOSpaceBar(dev->bar->bar, PhyAr, &phyar_value, 4);
    } while(~phyar_value & BIT(31));
}

void r8169_save_phy(r8169_device* dev)
{
    for (uint8_t offset = 0; offset < 0x20; offset++)
        dev->saved_phy_state[offset] = read_phy(dev, offset);
}

void r8169_resume_phy(r8169_device* dev)
{
    for (uint8_t offset = 0; offset < 0x20; offset++)
        write_phy(dev, offset, dev->saved_phy_state[offset]);
}

void r8169_reset(r8169_device* dev)
{
    if (!dev->suspended)
    {
        // This is a clean reinit, so we must initialize the IRQ object and DPC.

        Core_IrqObjectInitializeIRQL(&dev->irq, IRQL_R8169, true, true);
        dev->irq_res->irq->irq = &dev->irq;
        dev->irq_res->irq->masked = false;
        Drv_PCISetResource(dev->irq_res);
        dev->irq_res->irq->irq->irqChecker = r8169_irq_checker;
        dev->irq_res->irq->irq->handler = r8169_irq_handler;
        dev->irq_res->irq->irq->irqCheckerUserdata = dev;
        dev->irq_res->irq->irq->handlerUserdata = dev;

        dev->dpc.userdata = dev;

        dev->rx_buffer.envt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
        dev->free_descriptors_events[0] = EVENT_INITIALIZE(EVENT_NOTIFICATION);
        dev->free_descriptors_events[1] = EVENT_INITIALIZE(EVENT_NOTIFICATION);
        dev->free_descriptors_locks[0] = MUTEX_INITIALIZE();
        dev->free_descriptors_locks[1] = MUTEX_INITIALIZE();
        dev->rx_buffer_lock = PUSHLOCK_INITIALIZE();
        dev->tx_buffer_lock = PUSHLOCK_INITIALIZE();

        dev->magic = R8169_DEVICE_MAGIC;

        size_t len = snprintf(nullptr, 0, "r8169-eth%d", dev->idx);
        dev->interface_name = OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, len+1, nullptr);
        snprintf(dev->interface_name, len+1, "r8169-eth%d", dev->idx);
        dev->interface_name[len] = 0;

        dev->dev->resource_cmd_register->cmd_register |= 0x3; // io space + memspace
        Drv_PCISetResource(dev->dev->resource_cmd_register);

        r8169_alloc_set(dev, Rx_Set);
        r8169_alloc_set(dev, Tx_Set);
        // TODO: Test high-priority TX packets. 
        r8169_alloc_set(dev, TxH_Set);
    }

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

    DrvS_WriteIOSpaceBar(dev->bar->bar, ChipCmd, TX_ENABLE|RX_ENABLE, 1);
    r8169_init_rxcfg(dev);
    r8169_set_txcfg(dev);
    r8169_set_rxcfg_mode(dev);
    DrvS_WriteIOSpaceBar(dev->bar->bar, IntrMask, ENABLED_IRQS, 2);
    DrvS_WriteIOSpaceBar(dev->bar->bar, TimerInt, 0, 2);
}

static void* map_registers(uintptr_t phys, size_t size, bool uc, bool mmio)
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
        if (mmio)
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
        printf("info before assert: set=%d, dev->sets[%d]=0x%p", set, dev->sets[set]);
        OBOS_ASSERT(dev->sets[set] && "RTL8169: Attempt to reallocate set %d denied.");
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
    dev->sets[set] = map_registers(phys, nPages*OBOS_PAGE_SIZE, false, true);
    dev->sets_phys[set] = phys;

    phys = 0;

    for (size_t i = 0; i < DESCS_IN_SET; i++)
    {
        dev->sets[set][i].vlan = 0; // reserved probably, so we zero it.

        if (set == Rx_Set)
        {
            dev->sets[set][i].command = (RX_PACKET_SIZE & PACKET_LEN_MASK) & ~0x7;

            nPages = RX_PACKET_SIZE;
            if (nPages % OBOS_PAGE_SIZE)
                nPages += (OBOS_PAGE_SIZE-(nPages%OBOS_PAGE_SIZE));
            nPages /= OBOS_PAGE_SIZE;

            phys = Mm_AllocatePhysicalPages(nPages, 1, nullptr);
            dev->sets[set][i].buf = phys;
        }

        r8169_release_desc(dev, &dev->sets[set][i], set);
    }
    dev->sets[set][DESCS_IN_SET - 1].command |= EOR;
}

void r8169_release_desc(r8169_device* dev, r8169_descriptor* desc, uint8_t set)
{
    if (set == Rx_Set)
    {
        desc->command |= NIC_OWN;
        return; // We're done here.
    }

    desc->command = 0;
    desc->buf = 0;
    desc->vlan = 0;
    Core_MutexAcquire(&dev->free_descriptors_locks[set]);
    OBOS_ASSERT(set <= TxH_Set);
    r8169_descriptor_node* node = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(r8169_descriptor_node), nullptr);
    node->desc = desc;
    LIST_APPEND(r8169_descriptor_list, &dev->free_descriptors[set], node);
    Core_MutexRelease(&dev->free_descriptors_locks[set]);
}

r8169_descriptor* r8169_alloc_desc(r8169_device* dev, uint8_t set)
{
    if (set == Rx_Set)
        return nullptr; // We're done here.
    OBOS_ASSERT(Core_GetIrql() <= IRQL_DISPATCH);

    Core_MutexAcquire(&dev->free_descriptors_locks[set]);
    r8169_descriptor_node* node = LIST_GET_HEAD(r8169_descriptor_list, &dev->free_descriptors[set]);
    r8169_descriptor* desc = node->desc;
    LIST_REMOVE(r8169_descriptor_list, &dev->free_descriptors[set], node);
    Core_MutexRelease(&dev->free_descriptors_locks[set]);
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, node, sizeof(*node));

    return desc;
}

obos_status r8169_tx_queue_flush(r8169_device* dev, bool wait)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    if (wait)
        status = Core_PushlockAcquire(&dev->tx_buffer_lock, false);
    else
        status = Core_PushlockTryAcquire(&dev->tx_buffer_lock);
    if (obos_is_error(status))
        return status;

    r8169_frame* tx_frame = LIST_GET_HEAD(r8169_frame_list, &dev->tx_buffer.frames);

    // TODO: Do we need to synchronize this?
    uint8_t tx_poll = 0x00;
    while (tx_frame)
    {
        r8169_frame* next = LIST_GET_NEXT(r8169_frame_list, &dev->tx_buffer.frames, tx_frame);

        r8169_descriptor* desc = r8169_alloc_desc(dev, tx_frame->tx_priority_high ? TxH_Set : Tx_Set);
        if (!desc)
        {
            if (wait)
                Core_WaitOnObject(WAITABLE_OBJECT(dev->free_descriptors_events[tx_frame->tx_priority_high ? TxH_Set : Tx_Set]));
            else
                return OBOS_STATUS_WOULD_BLOCK;
        }

        uintptr_t phys = 0;
        MmS_QueryPageInfo(Mm_KernelContext.pt, (uintptr_t)tx_frame->buf, nullptr, &phys);
        irql oldIrql = Core_RaiseIrql(IRQL_R8169);
        OBOS_ENSURE(phys);
        desc->buf = phys;
        desc->command = tx_frame->sz & TX_PACKET_LEN_MASK;
        desc->command |= FS;
        desc->command |= LS;
        if (dev->ip_checksum_offload)
            desc->command |= IPCS;
        if (dev->udp_checksum_offload)
            desc->command |= UDPCS;
        if (dev->tcp_checksum_offload)
            desc->command |= TCPCS;
        desc->command |= NIC_OWN;
        if (tx_frame->tx_priority_high)
        {
            tx_poll |= BIT(7); // High priority frame.
            dev->tx_high_priority_awaiting_transfer += tx_frame->sz;
        }
        else
        {
            tx_poll |= BIT(6); // Normal priority frame.
            dev->tx_awaiting_transfer += tx_frame->sz;
        }
        Core_LowerIrql(oldIrql);
        // This does not free the physical memory.
        // That is done in r8169_tx
        Mm_VirtualMemoryFree(&Mm_KernelContext, tx_frame->buf, ((TX_PACKET_SIZE*128) + (OBOS_PAGE_SIZE-((TX_PACKET_SIZE*128)%OBOS_PAGE_SIZE))));

        r8169_buffer_remove_frame(&dev->tx_buffer, tx_frame);

        tx_frame = next;
    }
    DrvS_WriteIOSpaceBar(dev->bar->bar, TxPoll, tx_poll, 1);

    Core_PushlockRelease(&dev->tx_buffer_lock, false);
    return OBOS_STATUS_SUCCESS;
}

obos_status r8169_frame_tx_high_priority(r8169_frame* frame, bool priority)
{
    frame->tx_priority_high = !!priority;
    return OBOS_STATUS_SUCCESS;
}

LIST_GENERATE(r8169_frame_list, r8169_frame, node);
LIST_GENERATE(r8169_descriptor_list, r8169_descriptor_node, node);

obos_status r8169_frame_generate(r8169_device* dev, r8169_frame* frame, const void* data, size_t sz, uint32_t purpose)
{
    if (!dev->refcount)
    {
        dev->rx_dropped++;
        return OBOS_STATUS_SUCCESS;
    }
    switch (purpose)
    {
        case FRAME_PURPOSE_GENERAL: break;
        case FRAME_PURPOSE_RX:
        {
            if (obos_expect(sz > RX_PACKET_SIZE, false))
            {
                dev->rx_dropped++;
                return OBOS_STATUS_INVALID_ARGUMENT;
            }
            frame->refcount = dev->refcount;
            break;
        }
        case FRAME_PURPOSE_TX:
        {
            if (obos_expect(sz > (TX_PACKET_SIZE*128), false))
            {
                dev->tx_dropped++;
                return OBOS_STATUS_INVALID_ARGUMENT;
            }
            frame->refcount = 1;
            break;
        }
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    if (purpose == FRAME_PURPOSE_RX)
        frame->buf = OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, sz, nullptr);
    else
        frame->buf = map_registers(Mm_AllocatePhysicalPages(((TX_PACKET_SIZE*128) + (OBOS_PAGE_SIZE-((TX_PACKET_SIZE*128)%OBOS_PAGE_SIZE))) / OBOS_PAGE_SIZE, 1, nullptr), ((TX_PACKET_SIZE*128) + (OBOS_PAGE_SIZE-((TX_PACKET_SIZE*128)%OBOS_PAGE_SIZE))), false, true);
    memcpy(frame->buf, data, sz);
    frame->sz = sz;
    frame->idx = dev->rx_count;
    frame->purpose = purpose;
    return OBOS_STATUS_SUCCESS;
}

obos_status r8169_buffer_add_frame(r8169_buffer* buff, r8169_frame* frame)
{
    if (!frame->buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    r8169_frame* new_frame = OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(*frame), nullptr);
    *new_frame = *frame;
    LIST_APPEND(r8169_frame_list, &buff->frames, new_frame);
    return OBOS_STATUS_SUCCESS;
}

obos_status r8169_buffer_remove_frame(r8169_buffer* buff, r8169_frame* frame)
{
    if (!(--frame->refcount))
    {
        LIST_REMOVE(r8169_frame_list, &buff->frames, frame);
        if (frame->purpose == FRAME_PURPOSE_RX)
            OBOS_KernelAllocator->Free(OBOS_KernelAllocator, frame->buf, frame->sz);
        OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, frame, sizeof(*frame));
    }
    return OBOS_STATUS_SUCCESS;
}

obos_status r8169_buffer_read_next_frame(r8169_buffer* buff, r8169_frame** frame)
{
    if (!(*frame))
        *frame = LIST_GET_HEAD(r8169_frame_list, &buff->frames);
    else
        *frame = LIST_GET_NEXT(r8169_frame_list, &buff->frames, *frame);
    return OBOS_STATUS_SUCCESS;

}

obos_status r8169_buffer_poll(r8169_buffer* buff)
{
    while (!buff->envt.signaled)
        OBOSS_SpinlockHint();
    return OBOS_STATUS_SUCCESS;
}

obos_status r8169_buffer_block(r8169_buffer* buff)
{
    return Core_WaitOnObject(WAITABLE_OBJECT(buff->envt));
}
