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

    irq irq;
} r8169_device;

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
};
typedef struct r8169_descriptor {
    uint32_t command;
    uint32_t vlan;
    uint32_t buf_low;
    uint32_t buf_high;
} r8169_descriptor;

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
void r8169_alloc_set(r8169_device* dev, uint8_t set);
void r8169_free_set(r8169_device* dev, uint8_t set);
void r8169_read_mac(r8169_device* dev);
void r8169_hw_reset(r8169_device* dev);
void r8169_init_rxcfg(r8169_device *dev);
void r8169_set_rxcfg_mode(r8169_device *dev);
void r8169_set_txcfg(r8169_device *dev);
void r8169_lock_config(r8169_device* dev);
void r8169_unlock_config(r8169_device* dev);
