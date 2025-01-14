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
#define RX_PACKET_SIZE 0x1ff8

// The maximum amount of descriptors to allocate per set.
#define MAX_DESCS_IN_SET 0x400 /* 1024 */

// The amount of descriptors to allocate per set
#define DESCS_IN_SET MAX_DESCS_IN_SET

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

    r8169_descriptor* sets[3];
    uintptr_t sets_phys[3];

    bool suspended;

    irq irq;
} r8169_device;

enum {
    MAC0 = 0x0,
    MAC1 = 0x4,
    TxDescStartAddrLow = 0x20,
    TxDescStartAddrHigh = 0x24,
    TxHDescStartAddrLow = 0x28,
    TxHDescStartAddrHigh = 0x2c,
    ChipCmd = 0x37,
    TxPoll = 0x38,
    IntrMask = 0x3c,
    IntrStatus = 0x3e,
    RxConfig = 0x40,
    TxConfig = 0x44,
    Cfg9346 = 0x50,
    RxMaxSize = 0xda,
    RxDescAddrLow = 0xe4,
    RxDescAddrHigh = 0xe8,
    MaxTxPacketSize = 0xec,
};

void r8169_reset(r8169_device* dev);
void r8169_alloc_set(r8169_device* dev, uint8_t set);
void r8169_free_set(r8169_device* dev, uint8_t set);
