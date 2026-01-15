/*
 * drivers/x86_64/xhci/structs.h
 *
 * Copyright (c) 2026 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

#include <driver_interface/pci.h>

#include <irq/irq.h>
#include <irq/irql.h>
#include <irq/dpc.h>

typedef struct xhci_registers {
    uint8_t caplength;
    uint8_t resv1;
    uint16_t hciversion;
    uint32_t hcsparams1;
    uint32_t hcsparams2;
    uint32_t hcsparams3;
    uint32_t hccparams1;
    uint32_t dboff;
    uint32_t rtsoff;
    uint32_t hccparams2;
    char resv2[];
} OBOS_PACK xhci_registers;

enum {
    PORTSC_CSS = BIT(0),
    PORTSC_PED = BIT(1),
    PORTSC_OCA = BIT(3),
    PORTSC_PR = BIT(4),
    PORTSC_PLS = 0x1E0,
    PORTSC_PP = BIT(9),
    PORTSC_ROS = 0x3C00,
    PORTSC_PIC = 0xC000,
    PORTSC_LWS = BIT(16),
    PORTSC_CSC = BIT(17),
    PORTSC_PEC = BIT(18),
    PORTSC_WRC = BIT(19),
    PORTSC_OCC = BIT(20),
    PORTSC_PRC = BIT(21),
    PORTSC_PLC = BIT(22),
    PORTSC_CEC = BIT(23),
    PORTSC_CAS = BIT(24),
    PORTSC_WCE = BIT(25),
    PORTSC_WDE = BIT(26),
    PORTSC_WOE = BIT(27),
    PORTSC_DR = BIT(30),
    PORTSC_WPR = BIT(31),
};

enum {
    USB3_PORT_PMSC_U1_TIMEOUT_MASK = 0xFF,
    USB3_PORT_PMSC_U2_TIMEOUT_MASK = 0xFF00,
    USB3_PORT_PMSC_FLA = BIT(16),
};

enum {
    USB2_PORT_PMSC_L1S_MASK = 0x7,
    USB2_PORT_PMSC_RWE = BIT(3),
    USB2_PORT_PMSC_BESL_MAK = 0xf0,
    USB2_PORT_PMSC_L1_DEV_SLOT_MASK = 0xff00,
    USB2_PORT_PMSC_HLE = BIT(16),
    USB2_PORT_PMSC_PORT_TEST_CTRL_MASK = 0xF0000000,
};

enum {
    USB3_PORTLI_LINK_ERROR_COUNT_MASK = 0xffff,
    USB3_PORTLI_RLC_MASK = 0xF0000,
    USB3_PORTLI_TLC_MASK = 0xF00000,
};

typedef struct xhci_port_registers {
    uint32_t port_sc;
    uint32_t port_pmsc;
    uint32_t port_li;
    uint32_t port_hlpmc;
} OBOS_PACK xhci_port_registers;

enum {
    USBCMD_RUN = BIT(0),
    USBCMD_RESET = BIT(1),
    USBCMD_INTE = BIT(2),
    USBCMD_HSEE = BIT(3),
    USBCMD_LHCRST = BIT(7),
    USBCMD_CSS = BIT(8),
    USBCMD_CRS = BIT(9),
    USBCMD_EWE = BIT(10),
    USBCMD_EU3S = BIT(11),
    USBCMD_CME = BIT(13),
    USBCMD_ETE = BIT(14),
    USBCMD_TSC_EN = BIT(15),
    USBCMD_VTIOE = BIT(16),
};

enum {
    USBSTS_HCH = BIT(0),
    USBSTS_HSE = BIT(2),
    USBSTS_EINT = BIT(3),
    USBSTS_PCD = BIT(4),
    USBSTS_SSS = BIT(8),
    USBSTS_RSS = BIT(9),
    USBSTS_SRE = BIT(10),
    USBSTS_CNR = BIT(11),
    USBSTS_HCE = BIT(12),
};

enum {
    CRCR_RCS = BIT(0),
    CRCR_CS = BIT(1),
    CRCR_CA = BIT(2),
    CRCR_CRR = BIT(3),
    CRCR_CRP_MASK = 0xFFFFFFFFFFFFFFC0,
};

enum {
    DCBAAP_MASK = 0xFFFFFFFFFFFFFFC0,
};

enum {
    OP_CONFIG_MAX_SLOTS_EN_MASK = 0xff,
    OP_CONFIG_U3E = BIT(8),
    OP_CONFIG_CIE = BIT(9),
};

typedef struct xhci_op_registers {
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t pagesize;
    uint64_t resv1;
    uint32_t dnctrl;
    uint32_t crcr;
    uint64_t resv2[2];
    uint64_t dcbaap;
    uint64_t config;
    uint8_t resv3[0x3c4];
    xhci_port_registers ports[];
} OBOS_PACK xhci_op_registers;

typedef struct xhci_device {
    pci_device* dev;

    union {
        volatile void* base;
        volatile xhci_registers* cap_regs;
    };

    volatile xhci_op_registers* op_regs; 

    pci_resource* pci_bar;
    pci_resource* pci_irq;

    irq irq;
    dpc dpc;

    // Same bitfield as usbsts, but only the interrupt status bits
    uint32_t irqsts;

    bool did_bios_handoff : 1;
    bool has_64bit_support : 1;
    bool port_power_control_supported : 1;
    uint16_t xecp;

    struct xhci_device *next, *prev;
} xhci_device;
#ifndef INIT_C
extern struct {
#else
struct {
#endif
    xhci_device *head, *tail;
    size_t nNodes;
} g_devices;

void xhci_probe_bus(pci_bus* bus);
obos_status xhci_initialize_device(xhci_device* dev);
obos_status xhci_reset_device(xhci_device* dev);

OBOS_WEAK bool xhci_irq_checker(irq*, void*);
OBOS_WEAK void xhci_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql);

// true indicates a successful wait, false indicates timeout
bool poll_bit_timeout(volatile uint32_t *field, uint32_t mask, uint32_t expected, uint32_t us_timeout);

#define xhci_append_device(dev_) \
do {\
    typeof(dev_) _dev = (dev_);\
    if (!g_devices.head)\
        g_devices.head = _dev;\
    if (g_devices.tail)\
        g_devices.tail->next = _dev;\
    _dev->prev = g_devices.tail;\
    g_devices.tail = _dev;\
    g_devices.nNodes++;\
} while(0)

#if OBOS_IRQL_COUNT == 16
#	define IRQL_XHCI (7)
#elif OBOS_IRQL_COUNT == 8
#	define IRQL_XHCI (3)
#elif OBOS_IRQL_COUNT == 4
#	define IRQL_XHCI (2)
#elif OBOS_IRQL_COUNT == 2
#	define IRQL_XHCI (0)
#else
#	error Funny business.
#endif