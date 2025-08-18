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

#include <locks/event.h>
#include <locks/spinlock.h>

#include <net/eth.h>

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

enum {
    FRAME_PURPOSE_TX,
    FRAME_PURPOSE_RX,
    FRAME_PURPOSE_GENERAL,
};

typedef struct r8169_frame {
    void* buf;
    size_t sz;
    size_t idx;
    uint32_t purpose;
    bool tx_priority_high;
    _Atomic(size_t) refcount;
    LIST_NODE(r8169_frame_list, struct r8169_frame) node;
} r8169_frame;
typedef struct r8169_buffer {
    r8169_frame_list frames;
    event envt; // EVENT_NOTIFICATION
} r8169_buffer;
typedef struct r8169_descriptor {
    uint32_t command;
    uint32_t vlan;
    uint64_t buf;
} r8169_descriptor;

typedef LIST_HEAD(r8169_descriptor_list, struct r8169_descriptor_node) r8169_descriptor_list;
LIST_PROTOTYPE(r8169_descriptor_list, struct r8169_descriptor_node, node);
typedef struct r8169_descriptor_node {
    r8169_descriptor* desc;
    LIST_NODE(r8169_descriptor_list, struct r8169_descriptor_node) node;
} r8169_descriptor_node;

#define R8169_DEVICE_MAGIC 0x7186941C
#define R8169_HANDLE_MAGIC 0x7186941D

typedef struct r8169_device
{
    uint32_t magic;

    vnode* vn;

    pci_device *dev;
    pci_resource* bar; // BAR0
    pci_resource* irq_res;

    size_t idx;

    mac_address mac;
    char mac_readable[6*3+1]; // XX:XX:XX:XX:XX:XX\0

    bool ip_checksum_offload : 1;
    bool udp_checksum_offload : 1;
    bool tcp_checksum_offload : 1;

    r8169_descriptor* sets[3];
    uintptr_t sets_phys[3];

    bool suspended;

    _Atomic(size_t) refcount;

    irq irq;
    dpc dpc;
    uint16_t isr;

    char* interface_name;

    // Total number of received packets (both dropped, and undropped)
    size_t rx_count;
    // Total number of dropped packets
    size_t rx_dropped;
    // Total number of packet errors
    size_t rx_errors;
    // Total number of length errors
    size_t rx_length_errors;
    // Total number of CRC errors
    size_t rx_crc_errors;
    // Total number of bytes received.
    size_t rx_bytes;
    // received frames
    r8169_buffer rx_buffer;
    spinlock rx_buffer_lock;
    
    size_t tx_idx;
    size_t tx_priority_idx;

    // Total number of transmitted packets.
    size_t tx_count;
    // Total number of dropped packets that were to be transmitted.
    size_t tx_dropped;
    // Total number of bytes transmitted.
    size_t tx_bytes;
    // Total number of bytes waiting to be transferred.
    size_t tx_awaiting_transfer;
    // Total number of bytes from high priority packets waiting to be transferred.
    size_t tx_high_priority_awaiting_transfer;
    // Frames to transmit.
    r8169_buffer tx_buffer;
    spinlock tx_buffer_lock;

    uint16_t saved_phy_state[0x20];

    void(*data_ready)(void *userdata, void* vn, size_t bytes_ready);
    void* data_ready_userdata;
    thread* data_ready_thread;
} r8169_device;

typedef struct r8169_device_handle {
    uint32_t magic; // R8169_HANDLE_MAGIC

    r8169_device* dev;
    r8169_frame* rx_curr;
    size_t rx_off;
} r8169_device_handle;

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
    PhyAr = 0x60, // dword
    RxMaxSize = 0xda,
    CPlusCmd = 0xe0,
    IntrMitigate = 0xe2,
    RxDescAddrLow = 0xe4,
    RxDescAddrHigh = 0xe8,
    MaxTxPacketSize = 0xec,
};

#define TX_ENABLE BIT(2)
#define RX_ENABLE BIT(3)

enum {
    EOR = BIT(30),
    NIC_OWN = BIT(31),
    // Only valid if NIC_OWN is set
    FS = BIT(29),
    LS = BIT(28),
    MAR = BIT(27), // rx packet
    PAM = BIT(25), // rx packet
    BAR = BIT(24), // rx packet
    RWT_ERR = BIT(22), // rx packet
    RES_ERR = BIT(21), // rx packet
    RUNT_ERR = BIT(20), // rx packet
    CRC_ERR = BIT(19), // rx packet
    PID1 = BIT(18), // rx packet
    PID0 = BIT(17), // rx packet
    PID = PID1|PID0, // rx packet
    IPF = BIT(16), // rx packet
    UDPF = BIT(15), // rx packet
    TCPF = BIT(14), // rx packet
    // bits 0-13
    PACKET_LEN_MASK = 0x1fff,

    // bits 0-15
    TX_PACKET_LEN_MASK = 0xffff,
    LGSEND = BIT(27), // tx packet
    IPCS = BIT(18), // tx packet
    UDPCS = BIT(17), // tx packet
    TCPCS = BIT(16), // tx packet
};

enum {
    // Transimission set.
    Tx_Set,
    // Priority Transimission set.
    TxH_Set,
    // Receiving set.
    Rx_Set,
};

// Multiply by 128 to get the real size.
// NOTE: Do not do this when setting MaxTxPacketSize.
#define TX_PACKET_SIZE 0x3b

#define RX_PACKET_SIZE 0x1fff

// The maximum amount of descriptors to allocate per set.
#define MAX_DESCS_IN_SET 0x400 /* 1024 */

// The amount of descriptors to allocate per set
// TODO: Should we set this to something that would make this <= OBOS_PAGE_SIZE to facilitate
// allocation?
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
void r8169_save_phy(r8169_device* dev);
void r8169_resume_phy(r8169_device* dev);
void r8169_read_mac(r8169_device* dev);
void r8169_hw_reset(r8169_device* dev);
void r8169_init_rxcfg(r8169_device *dev);
void r8169_set_rxcfg_mode(r8169_device *dev);
void r8169_set_txcfg(r8169_device *dev);
void r8169_lock_config(r8169_device* dev);
void r8169_unlock_config(r8169_device* dev);
void r8169_rx(r8169_device* dev);
void r8169_tx(r8169_device* dev);
void r8169_set_irq_mask(r8169_device* dev, uint16_t mask);

void r8169_alloc_set(r8169_device* dev, uint8_t set);
void r8169_free_set(r8169_device* dev, uint8_t set);
r8169_descriptor* r8169_alloc_desc(r8169_device* dev, uint8_t set);
void r8169_release_desc(r8169_device* dev, r8169_descriptor* desc, uint8_t set);

obos_status r8169_frame_generate(r8169_device* dev, r8169_frame* frame, const void* data, size_t sz, uint32_t purpose);
obos_status r8169_frame_tx_high_priority(r8169_frame* frame, bool priority);
// *frame is copied into a local buffer.
obos_status r8169_buffer_add_frame(r8169_buffer* buff, r8169_frame* frame);
obos_status r8169_buffer_remove_frame(r8169_buffer* buff, r8169_frame* frame);
obos_status r8169_buffer_read_next_frame(r8169_buffer* buff, r8169_frame** frame);
obos_status r8169_buffer_poll(r8169_buffer* buff);
obos_status r8169_buffer_block(r8169_buffer* buff);
obos_status r8169_tx_queue_flush(r8169_device* dev, bool wait);
