/*
 * drivers/generic/freebsd-e1000/io.c
 *
 * Copyright (c) 2025-2026 Omar Berrow
 *
 * RX/TX function for the E1000 driver
*/

#include <int.h>
#include <error.h>

#include <e1000/e1000_hw.h>

#include "e1000_osdep.h"

#include <mm/pmm.h>
#include <mm/page.h>
#include <mm/context.h>

#include <irq/dpc.h>
#include <irq/timer.h>

#include <locks/event.h>

#include <allocators/base.h>

#include <utils/list.h>

#include <net/tables.h>
#include <net/eth.h>

#include <scheduler/schedule.h>

#include "dev.h"

DefineNetFreeSharedPtr

LIST_GENERATE(e1000_frame_list, e1000_frame, node);

#define EM_RADV 64
#define EM_RDTR 0

#define IGB_RX_PTHRESH \
  ((dev->hw.mac.type == e1000_i354) ? 12 : ((dev->hw.mac.type <= e1000_82576) ? 16 : 8))
#define IGB_RX_HTHRESH 8
#define IGB_RX_WTHRESH ((dev->hw.mac.type == e1000_82576) ? 1 : 4)
#define IGB_TX_PTHRESH ((dev->hw.mac.type == e1000_i354) ? 20 : 8)
#define IGB_TX_HTHRESH 1
#define IGB_TX_WTHRESH ((dev->hw.mac.type != e1000_82575) ? 1 : 16)

#define MAX_INTS_PER_SEC 8000
#define DEFAULT_ITR (1000000000 / (MAX_INTS_PER_SEC * 256))

static void e1000_init_rx_desc(e1000_device* dev)
{
    OBOS_STATIC_ASSERT((RX_QUEUE_SIZE * sizeof(union e1000_rx_desc_extended)) <= OBOS_PAGE_SIZE, "RX_QUEUE_SIZE is too large!");
    dev->rx_ring_phys_pg = MmH_PgAllocatePhysical(false, false);
    dev->rx_ring = dev->rx_ring_phys_pg->phys;
    union e1000_rx_desc_extended* desc = MmS_MapVirtFromPhys(dev->rx_ring);
    memzero(desc, sizeof(*desc) * RX_QUEUE_SIZE);
    for (size_t i = 0; i < RX_QUEUE_SIZE; i++)
    {
        dev->rx_ring_buffers[i] = Mm_AllocatePhysicalPages(1, 1, nullptr);
        desc[i].read.buffer_addr = dev->rx_ring_buffers[i];
        desc[i].wb.upper.status_error = 0;
    }
}

void e1000_init_rx(e1000_device* dev)
{
    e1000_init_rx_desc(dev);

    uint32_t rctl = E1000_READ_REG(&dev->hw, E1000_RCTL);

    // inspired by managarm

    if (dev->hw.mac.type != e1000_82574 && dev->hw.mac.type != e1000_82583)
        E1000_WRITE_REG(&dev->hw, E1000_RCTL, rctl & ~E1000_RCTL_EN);

    rctl &= ~(3 << E1000_RCTL_MO_SHIFT);
    rctl |= E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_LBM_NO | E1000_RCTL_RDMTS_HALF | (dev->hw.mac.mc_filter_type << E1000_RCTL_MO_SHIFT);
    rctl &= ~E1000_RCTL_SBP;
    rctl &= ~E1000_RCTL_UPE;
    rctl &= ~E1000_RCTL_MPE;
    rctl &= ~E1000_RCTL_LPE;
    rctl &= ~E1000_RCTL_SECRC;

    // taken from managarm
    if(dev->hw.mac.type >= e1000_82540) {
        E1000_WRITE_REG(&dev->hw, E1000_RADV, EM_RADV);
        /*
         * Set the interrupt throttling rate. Value is calculated
         * as DEFAULT_ITR = 1/(MAX_INTS_PER_SEC * 256ns)
         */
        E1000_WRITE_REG(&dev->hw, E1000_ITR, DEFAULT_ITR);
    }

    E1000_WRITE_REG(&dev->hw, E1000_RDTR, EM_RDTR);

    /* Use extended rx descriptor formats */
    u32 rfctl = E1000_READ_REG(&dev->hw, E1000_RFCTL);
    rfctl |= E1000_RFCTL_EXTEN;

    /*
    * When using MSIX interrupts we need to throttle
    * using the EITR register (82574 only)
    */
    if (dev->hw.mac.type == e1000_82574) {
        for (int i = 0; i < 4; i++) {
            E1000_WRITE_REG(&dev->hw, E1000_EITR_82574(i), DEFAULT_ITR);
        }
        /* Disable accelerated acknowledge */
        rfctl |= E1000_RFCTL_ACK_DIS;
    }

    E1000_WRITE_REG(&dev->hw, E1000_RFCTL, rfctl);
    // u32 rxcsum = E1000_READ_REG(&dev->hw, E1000_RXCSUM);
    // rxcsum &= ~E1000_RXCSUM_TUOFL;
    // rxcsum &= ~E1000_RXCSUM_IPOFL;
    E1000_WRITE_REG(&dev->hw, E1000_RXCSUM, 0);

    /*
     * XXX TEMPORARY WORKAROUND: on some systems with 82573
     * long latencies are observed, like Lenovo X60. This
     * change eliminates the problem, but since having positive
     * values in RDTR is a known source of problems on other
     * platforms another solution is being sought.
    */
    if (dev->hw.mac.type == e1000_82573) {
        E1000_WRITE_REG(&dev->hw, E1000_RDTR, 0x20);
    }

    // Ring slightly modified.
    E1000_WRITE_REG(&dev->hw, E1000_RDLEN(0), RX_QUEUE_SIZE * sizeof(union e1000_rx_desc_extended));
    E1000_WRITE_REG(&dev->hw, E1000_RDBAH(0), (u32)(dev->rx_ring >> 32));
    E1000_WRITE_REG(&dev->hw, E1000_RDBAL(0), (u32)dev->rx_ring);

    /*
     * Set PTHRESH for improved jumbo performance
     * According to 10.2.5.11 of Intel 82574 Datasheet,
     * RXDCTL(1) is written whenever RXDCTL(0) is written.
     * Only write to RXDCTL(1) if there is a need for different
     * settings.
     */
    if (dev->hw.mac.type == e1000_82574) {
        u32 rxdctl = E1000_READ_REG(&dev->hw, E1000_RXDCTL(0));

        rxdctl |= 0x20;    /* PTHRESH */
        rxdctl |= 4 << 8;  /* HTHRESH */
        rxdctl |= 4 << 16; /* WTHRESH */
        rxdctl |= 1 << 24; /* Switch to granularity */

        E1000_WRITE_REG(&dev->hw, E1000_RXDCTL(0), rxdctl);
    } else if (dev->hw.mac.type >= e1000_82575) {
        u32 srrctl = 2048 >> E1000_SRRCTL_BSIZEPKT_SHIFT;
        rctl |= E1000_RCTL_SZ_2048;

        /* Setup the Base and Length of the Rx Descriptor Rings */

        u32 rxdctl;
        srrctl |= E1000_SRRCTL_DESCTYPE_ADV_ONEBUF;
        E1000_WRITE_REG(&dev->hw, E1000_RDLEN(0), RX_QUEUE_SIZE * sizeof(struct e1000_rx_desc));
        E1000_WRITE_REG(&dev->hw, E1000_RDBAH(0), (uint32_t)(dev->rx_ring >> 32));
        E1000_WRITE_REG(&dev->hw, E1000_RDBAL(0), (uint32_t)dev->rx_ring);
        E1000_WRITE_REG(&dev->hw, E1000_SRRCTL(0), srrctl);

        /* Enable this Queue */
        rxdctl = E1000_READ_REG(&dev->hw, E1000_RXDCTL(0));
        rxdctl |= E1000_RXDCTL_QUEUE_ENABLE;
        rxdctl &= 0xFFF00000;
        rxdctl |= IGB_RX_PTHRESH;
        rxdctl |= IGB_RX_HTHRESH << 8;
        rxdctl |= IGB_RX_WTHRESH << 16;

        E1000_WRITE_REG(&dev->hw, E1000_RXDCTL(0), rxdctl);

        /* poll for enable completion */
        do {
            rxdctl = E1000_READ_REG(&dev->hw, E1000_RXDCTL(0));
        } while (!(rxdctl & E1000_RXDCTL_QUEUE_ENABLE));
    } else if (dev->hw.mac.type >= e1000_pch2lan) {
        e1000_lv_jumbo_workaround_ich8lan(&dev->hw, false);
    }

    /* Make sure VLAN Filters are off */
    rctl &= ~E1000_RCTL_VFE;

    if (dev->hw.mac.type < e1000_82575) {
        rctl |= E1000_RCTL_SZ_2048;
        /* ensure we clear use DTYPE of 00 here */
        rctl &= ~0x00000C00;
    }

    /* Setup the Head and Tail Descriptor Pointers */
    E1000_WRITE_REG(&dev->hw, E1000_RDH(0), 0);
    E1000_WRITE_REG(&dev->hw, E1000_RDT(0), RX_QUEUE_SIZE - 1);

    /* Write out the settings */
    E1000_WRITE_REG(&dev->hw, E1000_RCTL, rctl);
}

void e1000_init_tx(e1000_device* dev)
{
    dev->tx_ring_phys_pg = MmH_PgAllocatePhysical(false, false);
    dev->tx_ring = dev->tx_ring_phys_pg->phys;

    // Copied from managarm
    E1000_WRITE_REG(&dev->hw, E1000_TDLEN(0), TX_QUEUE_SIZE * sizeof(struct e1000_tx_desc));
    E1000_WRITE_REG(&dev->hw, E1000_TDBAH(0), (u32)(dev->tx_ring >> 32));
    E1000_WRITE_REG(&dev->hw, E1000_TDBAL(0), (u32)dev->tx_ring);

    /* Init the HEAD/TAIL indices */
    E1000_WRITE_REG(&dev->hw, E1000_TDH(0), 0);
    E1000_WRITE_REG(&dev->hw, E1000_TDT(0), 0);

    u32 txdctl = 0; /* clear txdctl */
    txdctl |= 0x1f; /* PTHRESH */
    txdctl |= 1 << 8; /* HTHRESH */
    txdctl |= 1 << 16; /* WTHRESH */
    txdctl |= 1 << 22; /* Reserved bit 22 must always be 1 */
    txdctl |= E1000_TXDCTL_GRAN;
    txdctl |= 1 << 25; /* LWTHRESH */

    E1000_WRITE_REG(&dev->hw, E1000_TXDCTL(0), txdctl);

    u32 tipg = 0;

    /* Set the default values for the Tx Inter Packet Gap timer */
    switch (dev->hw.mac.type) {
        case e1000_80003es2lan:
            tipg = DEFAULT_82543_TIPG_IPGR1;
            tipg |= DEFAULT_80003ES2LAN_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
            break;
        case e1000_82542:
            tipg = DEFAULT_82542_TIPG_IPGT;
            tipg |= DEFAULT_82542_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT;
            tipg |= DEFAULT_82542_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
            break;
        default:
            if ((dev->hw.phy.media_type == e1000_media_type_fiber) ||
            (dev->hw.phy.media_type == e1000_media_type_internal_serdes))
                tipg = DEFAULT_82543_TIPG_IPGT_FIBER;
            else
                tipg = DEFAULT_82543_TIPG_IPGT_COPPER;
            tipg |= DEFAULT_82543_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT;
            tipg |= DEFAULT_82543_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
    }

    E1000_WRITE_REG(&dev->hw, E1000_TIPG, tipg);
    E1000_WRITE_REG(&dev->hw, E1000_TIDV, 0);

    if (dev->hw.mac.type >= e1000_82540)
        E1000_WRITE_REG(&dev->hw, E1000_TADV, 0);

    u32 tarc = 0;

#define TARC_SPEED_MODE_BIT (1 << 21) /* On PCI-E MACs only */
#define TARC_ERRATA_BIT (1 << 26)     /* Note from errata on 82574 */

    if ((dev->hw.mac.type == e1000_82571) || (dev->hw.mac.type == e1000_82572)) {
        tarc = E1000_READ_REG(&dev->hw, E1000_TARC(0));
        tarc |= TARC_SPEED_MODE_BIT;
        E1000_WRITE_REG(&dev->hw, E1000_TARC(0), tarc);
    } else if (dev->hw.mac.type == e1000_80003es2lan) {
        /* errata: program both queues to unweighted RR */
        tarc = E1000_READ_REG(&dev->hw, E1000_TARC(0));
        tarc |= 1;
        E1000_WRITE_REG(&dev->hw, E1000_TARC(0), tarc);
        tarc = E1000_READ_REG(&dev->hw, E1000_TARC(1));
        tarc |= 1;
        E1000_WRITE_REG(&dev->hw, E1000_TARC(1), tarc);
    } else if (dev->hw.mac.type == e1000_82574) {
        tarc = E1000_READ_REG(&dev->hw, E1000_TARC(0));
        tarc |= TARC_ERRATA_BIT;
        E1000_WRITE_REG(&dev->hw, E1000_TARC(0), tarc);
    }

    /* Program the Transmit Control Register */
    u32 tctl = E1000_READ_REG(&dev->hw, E1000_TCTL);
    tctl &= ~E1000_TCTL_CT;
    tctl |= (E1000_TCTL_RTLC | E1000_TCTL_EN | E1000_TCTL_PSP | (E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT));

    if (dev->hw.mac.type >= e1000_82571)
        tctl |= E1000_TCTL_MULR;

    /* This write will effectively turn on the transmit unit. */
    E1000_WRITE_REG(&dev->hw, E1000_TCTL, tctl);

    /* SPT and KBL errata workarounds */
    if (dev->hw.mac.type == e1000_pch_spt) {
        u32 reg;
        reg = E1000_READ_REG(&dev->hw, E1000_IOSFPC);
        reg |= E1000_RCTL_RDMTS_HEX;
        E1000_WRITE_REG(&dev->hw, E1000_IOSFPC, reg);
        /* i218-i219 Specification Update 1.5.4.5 */
        reg = E1000_READ_REG(&dev->hw, E1000_TARC(0));
        reg &= ~E1000_TARC0_CB_MULTIQ_3_REQ;
        reg |= E1000_TARC0_CB_MULTIQ_2_REQ;
        E1000_WRITE_REG(&dev->hw, E1000_TARC(0), reg);
    }

    struct e1000_tx_desc* desc = ((struct e1000_tx_desc*)MmS_MapVirtFromPhys(dev->tx_ring));
    for (size_t i = 0; i < TX_QUEUE_SIZE; i++)
    {
        dev->tx_buffers[i] = Mm_AllocatePhysicalPages(TX_BUFFER_PAGES, 1, nullptr);
        desc[i].buffer_addr = dev->tx_buffers[i];
        desc[i].lower.data = 0;
        desc[i].upper.data = 0;
    }
    OBOS_ENSURE(memcmp_b(desc, 0, sizeof(*desc)*TX_QUEUE_SIZE) == false);
}

void e1000_tx_reap(e1000_device* dev);

event* e1000_tx_packet(e1000_device* dev, const void* buffer, size_t size, bool dry, obos_status* status)
{
    e1000_tx_reap(dev);
    volatile struct e1000_tx_desc* desc = &((volatile struct e1000_tx_desc*)MmS_MapVirtFromPhys(dev->tx_ring))[dev->tx_index % TX_QUEUE_SIZE];
    if (dry)
        return nullptr;
    irql oldIrql = Core_RaiseIrql(IRQL_E1000);
    
    size_t nPages = size / OBOS_PAGE_SIZE;
    if (size % OBOS_PAGE_SIZE)
        nPages++;
    if (nPages > TX_BUFFER_PAGES)
    {
        if (status)
            *status = OBOS_STATUS_MESSAGE_TOO_BIG;
        Core_LowerIrql(oldIrql);
        return nullptr;
    }

    uintptr_t buff = dev->tx_buffers[dev->tx_index % TX_QUEUE_SIZE];
    memcpy(MmS_MapVirtFromPhys(buff), buffer, size);
    desc->buffer_addr = buff;
    desc->upper.data = 0;
    desc->lower.data = size | E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
    E1000_WRITE_REG(&dev->hw, E1000_TDBAH(0), (u32)(dev->tx_ring >> 32));
    E1000_WRITE_REG(&dev->hw, E1000_TDBAL(0), (u32)dev->tx_ring);
    E1000_WRITE_REG(&dev->hw, E1000_TDT(0), ++dev->tx_index % TX_QUEUE_SIZE);
  
    Core_LowerIrql(oldIrql);
    
    uintptr_t deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(1*1000*1000);
    while (~desc->upper.data & E1000_TXD_STAT_DD && CoreS_GetTimerTick() < deadline)
        Core_Yield();
    if (~desc->upper.data & E1000_TXD_STAT_DD)
        *status = OBOS_STATUS_TIMED_OUT;

    return nullptr;
}


static void rx_dpc(dpc* d, void* udata)
{
    OBOS_UNUSED(d);
    e1000_device* dev = udata;
    
    vnode* const nic = dev->vn;

    e1000_frame* current_frame = nullptr;
    size_t offset = 0;
        
    while (true)
    {
        uint32_t length = 0;
        bool eop = false;
        if(dev->hw.mac.type >= e1000_82547)
        {
            union e1000_rx_desc_extended* desc = &((union e1000_rx_desc_extended*)MmS_MapVirtFromPhys(dev->rx_ring))[(dev->rx_idx % RX_QUEUE_SIZE)];
            if (~desc->wb.upper.status_error & E1000_RXD_STAT_DD)
                break;
            length = desc->wb.upper.length;
            eop = desc->wb.upper.status_error & E1000_RXD_STAT_EOP;
            desc->wb.upper.status_error = 0;
            desc->read.buffer_addr = dev->rx_ring_buffers[(dev->rx_idx % RX_QUEUE_SIZE)];
        }
        else
        {
            struct e1000_rx_desc* desc = &((struct e1000_rx_desc*)MmS_MapVirtFromPhys(dev->rx_ring))[(dev->rx_idx % RX_QUEUE_SIZE)];
            if (~desc->status & E1000_RXD_STAT_DD)
                break;
            eop = desc->status & E1000_RXD_STAT_EOP;
            length = desc->length;
            desc->status = 0;
        }
        if (!current_frame)
            current_frame = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(e1000_frame), nullptr);
        current_frame->size += length;
        current_frame->buff = Reallocate(OBOS_NonPagedPoolAllocator, current_frame->buff, current_frame->size, current_frame->size - length, nullptr);
        current_frame->refs = dev->refs;
        memcpy((char*)current_frame->buff + offset, MmS_MapVirtFromPhys(dev->rx_ring_buffers[(dev->rx_idx % RX_QUEUE_SIZE)]), length);
        if (eop)
        {
            if (current_frame->size < 14)
            {
                NetError("e1000: dropping misplaced runt!\n");
                Free(OBOS_NonPagedPoolAllocator, current_frame->buff, current_frame->size);
                Free(OBOS_NonPagedPoolAllocator, current_frame, sizeof(e1000_frame));
                current_frame = nullptr;
                offset = 0;
                ++dev->rx_idx;
                continue;
            }

            if (nic->net_tables)
            {
                shared_ptr* buf = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(shared_ptr), nullptr);
                OBOS_SharedPtrConstructSz(buf, current_frame->buff, current_frame->size);
                buf->free = OBOS_SharedPtrDefaultFree;
                buf->onDeref = NetFreeSharedPtr;
                buf->freeUdata = OBOS_NonPagedPoolAllocator;
                current_frame->refs--;
                if (!current_frame->refs)
                    Free(OBOS_NonPagedPoolAllocator, current_frame, sizeof(e1000_frame));
    
                Net_EthernetProcess(nic, 0, OBOS_SharedPtrCopy(buf), buf->obj, buf->szObj, nullptr);
            }

            LIST_APPEND(e1000_frame_list, &dev->rx_frames, current_frame);
            current_frame = nullptr;
            offset = 0;
        }
        else
            offset += length;

        ++dev->rx_idx;
    }
    E1000_WRITE_REG(&dev->hw, E1000_RDT(0), ((dev->rx_idx-1) % RX_QUEUE_SIZE));
    if (nic->net_tables)
        Net_TCPFlushACKs(nic->net_tables);
    Core_EventSet(&dev->rx_evnt, false);
}
void e1000_rx(e1000_device* dev)
{
    dev->dpc.userdata = dev;
    CoreH_InitializeDPC(&dev->dpc, rx_dpc, Core_DefaultThreadAffinity);
}

static void tx_event_set_dpc(dpc* d, void* udata)
{
    OBOS_UNUSED(d);
    e1000_device* dev = udata;
    Core_EventSet(&dev->tx_done_evnt, false);
}
void e1000_tx_reap(e1000_device* dev)
{
    volatile struct e1000_tx_desc* desc = &((struct e1000_tx_desc*)MmS_MapVirtFromPhys(dev->tx_ring))[0];
    size_t i = dev->tx_index;
    for (; ; )
    {
        if (~desc[i].upper.data & E1000_TXD_STAT_DD)
            break;
        desc[i].upper.fields.status = 0;   
        desc[i].lower.data = 0;
        desc[i].buffer_addr = 0;
        i++;
    }
    dev->tx_index = i;
}

void e1000_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_UNUSED(i && frame && oldIrql);
    e1000_device* dev = userdata;
    if (dev->icr & E1000_ICR_RXT0)
        e1000_rx(dev);
    dev->icr = 0;
}

bool e1000_check_irq_callback(struct irq* i, void* userdata)
{
    OBOS_UNUSED(i);
    e1000_device* dev = userdata;
    dev->icr |= E1000_READ_REG(&dev->hw, E1000_ICR);
    return dev->icr != 0;
}