/*
 * oboskrnl/driver_interface/pci.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

#include <irq/irq.h>

#include <uacpi/types.h>

#include <utils/list.h>

typedef enum pci_iteration_decision
{
    PCI_ITERATION_DECISION_ABORT,
    PCI_ITERATION_DECISION_CONTINUE,
} pci_iteration_decision;

typedef struct pci_device_location
{
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
} pci_device_location;

typedef union pci_hid
{
    struct
    {
        uint8_t classCode;
        uint8_t subClass;
        uint8_t progIf;
        uint8_t resv;
        uint16_t vendorId;
        uint16_t deviceId;
    } OBOS_PACK indiv;
    uint64_t id;
} pci_hid;

typedef enum {
    PCI_BARIO,
    PCI_BAR32,
    PCI_BAR64,
} pci_bar_type;
#define PCI_BAR_MASK_32BIT (0b000 /* Memory Space BAR, 32-bit */)
#define PCI_BAR_MASK_64BIT (0b100 /* Memory Space BAR, 64-bit */)
#define PCI_BAR_MASK_IOSPACE (0b001 /* IOSpace BAR */)
typedef struct pci_bar
{
    uint8_t idx;
    union {
        // We don't use uintptr_t here in case that is 32-bit, but a PCI device has a 64-bit BAR
        // This can happen on a kernel compiled for i686 running on an x86-64 host.
        uint64_t phys;
        uint16_t iospace;
    };
    size_t size;
    pci_bar_type type;
    bool prefetchable;
} pci_bar;

typedef struct pci_capability
{
    struct pci_capability* next_cap;
    uint8_t id;
    uint8_t offset;
} pci_capability;

typedef enum {
    PCI_RESOURCE_INVALID,
    PCI_RESOURCE_BAR,
    PCI_RESOURCE_CMD_REGISTER,
    PCI_RESOURCE_IRQ,
    PCI_RESOURCE_CAPABILITY,
} pci_resource_type;
typedef struct pci_resource
{
    union {
        pci_bar* bar;
        uint16_t cmd_register;
        struct pci_irq_handle* irq;
        pci_capability* cap;
    };
    pci_resource_type type;
    struct pci_device* owner;

    LIST_NODE(pci_resource_list, struct pci_resource) node;
} pci_resource;

typedef LIST_HEAD(pci_resource_list, pci_resource) pci_resource_list;
LIST_PROTOTYPE(pci_resource_list, pci_resource, node);

typedef struct pci_device {
    // Location info
    pci_device_location location;
    struct pci_bus* owner;

    // hid
    pci_hid hid;

    // Resource info
    pci_resource_list resources;

    // Resource shortcuts
    pci_capability* first_capability;
    pci_resource* resource_cmd_register; // command register

    LIST_NODE(pci_device_list, struct pci_device) node;
} pci_device;

typedef LIST_HEAD(pci_device_list, pci_device) pci_device_list;
LIST_PROTOTYPE(pci_device_list, pci_device, node);

typedef struct pci_bus {
    uint8_t busNumber;
    pci_device_list devices;
    struct uacpi_namespace_node* acpiNode;
} pci_bus;

extern OBOS_EXPORT pci_bus Drv_PCIBuses[256];
extern OBOS_EXPORT size_t Drv_PCIBusCount;

// Only initializes Bus 0.
// CAN and SHOULD be called before uACPI UACPI_INIT_LEVEL_NAMESPACE_LOADED
obos_status Drv_EarlyPCIInitialize();
// Initializes all buses.
// Should be called after UACPI_INIT_LEVEL_NAMESPACE_LOADED.
obos_status Drv_PCIInitialize();

// Writes a resource to the appropriate place in the PCI config space.
OBOS_EXPORT obos_status Drv_PCISetResource(const pci_resource* resource);
// Gets a resource from the appropriate place in the PCI config space.
OBOS_EXPORT obos_status Drv_PCIUpdateResource(pci_resource* resource);

typedef struct pci_irq_handle
{
    union {
        uint32_t arch_handle; // from DrvS_RegisterIRQPin
        uintptr_t msix_entry; // a pointer to the MSI-X table entry.
    } un;
    // If zero, assume arch_handle is valid.
    // If non-zero, check msix_entry.
    // If zero, this is an MSI-device, otherwise the device utilizes MSI-X.
    pci_capability* msi_capability;
    pci_device* dev;
    uintptr_t msix_pending_entry;
    irq* irq;
    bool initialized;
    bool masked;
} pci_irq_handle;
OBOS_WEAK bool DrvS_CheckIrqCallbackIrqPin(struct irq* i, void* userdata);
#if OBOS_ARCHITECTURE_HAS_PCI
OBOS_EXPORT obos_status DrvS_WriteIOSpaceBar(pci_bar* bar, uint16_t offset, uint32_t val, uint8_t byte_width);
OBOS_EXPORT obos_status DrvS_ReadIOSpaceBar(pci_bar* bar, uint16_t offset, uint32_t *val, uint8_t byte_width);
OBOS_EXPORT obos_status DrvS_EnumeratePCI(uint8_t bus, pci_iteration_decision(*cb)(void* udata, pci_device_location device), void *cb_udata);
OBOS_EXPORT obos_status DrvS_ReadPCIRegister(pci_device_location loc, uint8_t offset, size_t accessSize, uint64_t* val);
OBOS_EXPORT obos_status DrvS_WritePCIRegister(pci_device_location loc, uint8_t offset, size_t accessSize, uint64_t val);
OBOS_EXPORT size_t DrvS_GetBarSize(pci_device_location loc, uint8_t barIndex, bool is64bit, obos_status* status);
// Returns the address field that MSI(-X) expects, and outputs in *data the data expected by MSI(-X).
uint64_t DrvS_MSIAddressAndData(uint64_t* data, irq_vector_id vec, uint32_t processor, bool edgetrigger, bool deassert);
obos_status DrvS_RegisterIRQPin(const pci_device_location* dev, uint32_t* handle, irq_vector_id vector);
obos_status DrvS_MaskIRQPin(uint32_t handle, bool mask);
#ifdef __x86_64__
#   define PCI_IRQ_CAN_USE_ACPI 1 /* for stuff like PCI routing tables */
#   define PCI_IRQ_UACPI_INIT_LEVEL UACPI_INIT_LEVEL_NAMESPACE_LOADED
#endif
// Note: Overwrites irq->irqChecker as well as irq->irqCheckerUserdata.
// It also overwrites the IRQ move callback.
obos_status Drv_UpdatePCIIrq(irq* irq, pci_device* dev, pci_irq_handle* handle);
#else
OBOS_WEAK obos_status DrvS_EnumeratePCI(pci_iteration_decision(*cb)(void* udata, pci_device_node device), void *cb_udata);
OBOS_WEAK obos_status DrvS_ReadPCIDeviceNode(pci_device_location loc, pci_device_node* node);
OBOS_WEAK obos_status DrvS_ReadPCIRegister(pci_device_location loc, uint8_t offset, size_t accessSize, uint64_t* val);
OBOS_WEAK obos_status DrvS_WritePCIRegister(pci_device_location loc, uint8_t offset, size_t accessSize, uint64_t val);
OBOS_WEAK size_t DrvS_GetBarSize(pci_device_location loc, uint8_t barIndex, bool is64bit, size_t* size);
OBOS_WEAK uint64_t DrvS_MSIAddressAndData(uint64_t* data, uint8_t vec, uint32_t processor, bool edgetrigger, bool deassert);
OBOS_WEAK obos_status DrvS_RegisterIRQPin(const pci_device_node* dev, uint32_t* handle, irq_vector_id vector);
OBOS_WEAK obos_status DrvS_MaskIRQPin(uint32_t handle, bool mask);
typedef struct pci_irq_handle
{
    uint8_t unused;
} pci_irq_handle;
OBOS_WEAK obos_status Drv_RegisterPCIIrq(irq* irq, const pci_device_node* dev, pci_irq_handle* handle);
OBOS_WEAK obos_status Drv_MaskPCIIrq(const pci_irq_handle* handle, bool mask);
#endif

#ifndef PCI_IRQ_CAN_USE_ACPI
#   define PCI_IRQ_CAN_USE_ACPI 0
#endif

#ifndef PCI_IRQ_UACPI_INIT_LEVEL
#   if !PCI_IRQ_CAN_USE_ACPI
#       define PCI_IRQ_UACPI_INIT_LEVEL 0
#   else
#       define PCI_IRQ_UACPI_INIT_LEVEL UACPI_INIT_LEVEL_NAMESPACE_LOADED
#   endif
#endif
