/*
 * drivers/generic/ehci/structs.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

#include <driver_interface/pci.h>

#include <irq/irq.h>
#include <irq/dpc.h>

#include <locks/event.h>
#include <locks/spinlock.h>

typedef volatile struct ehci_base_registers {
    uint8_t caplength;
    uint8_t resv;
    uint16_t hciversion;
    uint32_t hcsparams;
    uint32_t hccparams;
    uint64_t hcsp_portroute;
    char operational_base[];
} OBOS_PACK ehci_base_registers;
typedef volatile struct ehci_operational_base_registers {
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t usbintr;
    uint32_t frindex;
    uint32_t ctrldsssegment;
    uint32_t periodiclistbase;
    uint32_t asynclistaddr;
    uint32_t resv1[9];
    uint32_t configflag;
    uint32_t portsc[];
} OBOS_PACK ehci_operational_base_registers;

typedef struct ehci_port {
    void* resv;
} ehci_port, *ehci_ports;

typedef struct ehci_controller {
    pci_device* dev;
    
    union {
        ehci_base_registers* base_reg;
        void* bar_base;
    };
    ehci_operational_base_registers* op_base_reg;
    pci_resource* bar_resource;

    irq irq;
    pci_resource* irq_resource;
    dpc exec_dpc;
    // Only the bottom 6 bits
    uint32_t usbsts; 
    
    spinlock controller_lock;
    
    ehci_ports ports;
    size_t nPorts;

    bool suspended;
    bool initialized;

    struct {
        uintptr_t phys;
        uint32_t* virt; 
    } periodicList;
} ehci_controller, *ehci_controllers;
extern ehci_controllers g_controllers;
extern size_t g_controller_count;

#if OBOS_IRQL_COUNT == 16
#	define IRQL_EHCI (7)
#elif OBOS_IRQL_COUNT == 8
#	define IRQL_EHCI (3)
#elif OBOS_IRQL_COUNT == 4
#	define IRQL_EHCI (2)
#elif OBOS_IRQL_COUNT == 2
#	define IRQL_EHCI (0)
#else
#	error Funny business.
#endif

void ehci_initialize_controller(ehci_controller* controller);
void ehci_reset_controller(ehci_controller* controller);
uintptr_t ehci_alloc_phys(size_t nPages);

bool ehci_irq_check(irq* i, void* userdata);
void ehci_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql);

bool wait_bit_timeout(volatile uint32_t* field, uint32_t mask, uint32_t expected, uint32_t us_timeout);