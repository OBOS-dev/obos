/*
 * drivers/generic/r8169/structs.h
 *
 * Copyright (c) 2025 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>

#include <driver_interface/pci.h>

#include <irq/irq.h>
#include <irq/dpc.h>

#include <locks/pushlock.h>
#include <locks/event.h>

#include <utils/list.h>

#if OBOS_IRQL_COUNT == 16
#	define IRQL_R8169 (7)
#elif OBOS_IRQL_COUNT == 8
#	define IRQL_R8169 (3)
#elif OBOS_IRQL_COUNT == 4
#	define IRQL_R8169 (2)
#elif OBOS_IRQL_COUNT == 2
#	define IRQL_R8169 (0)
#else
#	error Funny business.
#endif

typedef LIST_HEAD(r8169_frame_list, struct r8169_frame) r8169_frame_list;
LIST_PROTOTYPE(r8169_frame_list, struct r8169_frame, node);
typedef struct r8169_frame {
    void* buf;
    size_t sz;
    size_t idx;
    _Atomic(size_t) refcount;
    LIST_NODE(r8169_frame_list, struct r8169_frame) node;
} r8169_frame;
typedef struct r8169_buffer {
    r8169_frame_list frames;
    event envt; // EVENT_NOTIFICATION
} r8169_buffer;
typedef struct r8169_device
{
    pci_device *dev;
    pci_resource* bar; // BAR0
    pci_resource* irq_res;

    uint8_t mac[6];
    char mac_readable[6*3+1]; // XX:XX:XX:XX:XX:XX\0

    struct r8169_descriptor* sets[3];
    uintptr_t sets_phys[3];

    bool suspended;

    _Atomic(size_t) refcount;

    irq irq;
    dpc dpc;
    uint16_t isr;

    // total number of received packets (both dropped, and undropped)
    size_t rx_count;
    // total number of dropped packets
    size_t rx_dropped;
    // total number of packet errors
    size_t rx_errors;
    // total number of length errors
    size_t rx_length_errors;
    // total number of CRC errors
    size_t rx_crc_errors;
    // total number of bytes received.
    size_t rx_bytes;
    // received frames
    r8169_buffer rx_buffer;
    pushlock rx_buffer_lock;
} r8169_device;
typedef struct r8169_descriptor {
    uint32_t command;
    uint32_t vlan;
    uint64_t buf;
} r8169_descriptor;

enum {
    MAC0 = 0x0,
    MAC1 = 0x4,
    MAR0 = 0x8,
    TxDescStartAddrLow = 0x20,
    TxDescStartAddrHigh = 0x24,
    TxHDescStartAddrLow = 0x28,
    TxHDescStartAddrHigh = 0x2c,
    ChipCmd = 0x37,
    TxPoll = 0x38,
    IntrMask = 0x3c,
    IntrStatus = 0x3e,
    TxConfig = 0x40,
    RxConfig = 0x44,
    TimerCount = 0x48,
    MissedPacketCount = 0x4c,
    Cfg9346 = 0x50,
    TimerInt = 0x58,
    RxMaxSize = 0xda,
    CPlusCmd = 0xe0,
    IntrMitigate = 0xe2,
    RxDescAddrLow = 0xe4,
    RxDescAddrHigh = 0xe8,
    MaxTxPacketSize = 0xec,
};

enum {
    EOR = BIT(30),
    NIC_OWN = BIT(31),
    // Only valid if NIC_OWN is set
    FS = BIT(29),
    LS = BIT(28),
    MAR = BIT(27),
    PAM = BIT(25),
    BAR = BIT(24),
    RWT_ERR = BIT(22),
    RES_ERR = BIT(21),
    RUNT_ERR = BIT(20),
    CRC_ERR = BIT(19),
    PID1 = BIT(18),
    PID0 = BIT(17),
    PID = PID1|PID0,
    IPF = BIT(16),
    UDPF = BIT(15),
    TCPF = BIT(14),
    // bits 0-13
    PACKET_LEN_MASK = 0x1fff,
};

enum {
    // Transimission set.
    Tx_Set,
    // Priority Transimission set.
    TxH_Set,
    // Receiving set.
    Rx_Set,
};

#define TX_PACKET_SIZE 0x3b
#define RX_PACKET_SIZE 0x1fff

// The maximum amount of descriptors to allocate per set.
#define MAX_DESCS_IN_SET 0x400 /* 1024 */

// The amount of descriptors to allocate per set
#define DESCS_IN_SET MAX_DESCS_IN_SET

#define RxOk BIT(0)
#define RxErr BIT(1)
#define TxOk BIT(2)
#define TxErr BIT(3)
#define RxOverflow BIT(4)
#define LinkStatus BIT(5)

#define Cfg9346_Lock 0x00
#define Cfg9346_Unlock 0xc0

#define ENABLED_IRQS RxOk|RxErr|TxOk|TxErr|LinkStatus|RxOverflow

void r8169_reset(r8169_device* dev);
void r8169_read_mac(r8169_device* dev);
void r8169_hw_reset(r8169_device* dev);
void r8169_init_rxcfg(r8169_device *dev);
void r8169_set_rxcfg_mode(r8169_device *dev);
void r8169_set_txcfg(r8169_device *dev);
void r8169_lock_config(r8169_device* dev);
void r8169_unlock_config(r8169_device* dev);
void r8169_rx(r8169_device* dev);
void r8169_set_irq_mask(r8169_device* dev, uint16_t mask);

void r8169_alloc_set(r8169_device* dev, uint8_t set);
void r8169_free_set(r8169_device* dev, uint8_t set);
void r8169_release_desc(r8169_descriptor* desc);

void r8169_frame_generate(r8169_device* dev, r8169_frame* frame, void* data, size_t sz);
// *frame is copied into a local buffer.
void r8169_buffer_add_frame(r8169_buffer* buff, r8169_frame* frame);
void r8169_buffer_remove_frame(r8169_buffer* buff, r8169_frame* frame);
void r8169_buffer_read_next_frame(r8169_buffer* buff, r8169_frame** frame);
void r8169_buffer_poll(r8169_buffer* buff);
void r8169_buffer_block(r8169_buffer* buff);
