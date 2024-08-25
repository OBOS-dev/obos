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

typedef enum pci_iteration_decision
{
    PCI_ITERATION_DECISION_ABORT,
    PCI_ITERATION_DECISION_CONTINUE,
} pci_iteration_decision;
typedef union pci_device
{
    struct
    {
        uint8_t classCode;
        uint8_t subClass;
        uint8_t progIf;
        uint8_t revId;
        uint16_t vendorId;
        uint16_t deviceId;
    } OBOS_PACK indiv;
    uint64_t id;
} pci_device;
typedef union pci_bar
{
    struct
    {
        uint32_t bar0;
        uint32_t bar1;
        uint32_t bar2;
        uint32_t bar3;
        uint32_t bar4;
        uint32_t bar5;
    } indiv32;
    struct
    {
        uint64_t bar0;
        uint64_t bar1;
        uint64_t bar2;
    } indiv64;
    uint32_t arr32[6];
} pci_bar;
typedef struct pci_irq_info
{
    uint8_t int_line;
    uint8_t int_pin;
} pci_irq_info;
typedef struct pci_device_location
{
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
} pci_device_location;
typedef struct pci_device_node
{
    pci_device_location info;

    pci_device device;

    pci_bar bars;

    pci_irq_info irq;
} pci_device_node;
OBOS_WEAK bool DrvS_CheckIrqCallbackIrqPin(struct irq* i, void* userdata);
#if OBOS_ARCHITECTURE_HAS_PCI
OBOS_EXPORT obos_status DrvS_EnumeratePCI(pci_iteration_decision(*cb)(void* udata, pci_device_node device), void *cb_udata);
OBOS_EXPORT obos_status DrvS_ReadPCIDeviceNode(pci_device_location loc, pci_device_node* node);
OBOS_EXPORT obos_status DrvS_ReadPCIRegister(pci_device_location loc, uint8_t offset, size_t accessSize, uint64_t* val);
OBOS_EXPORT obos_status DrvS_WritePCIRegister(pci_device_location loc, uint8_t offset, size_t accessSize, uint64_t val);
OBOS_EXPORT size_t DrvS_GetBarSize(pci_device_location loc, uint8_t barIndex, bool is64bit, obos_status* status);
// Returns the address field that MSI(-X) expects, and outputs in *data the data expected by MSI(-X).
uint64_t DrvS_MSIAddressAndData(uint64_t* data, irq_vector_id vec, uint32_t processor, bool edgetrigger, bool deassert);
obos_status DrvS_RegisterIRQPin(const pci_device_node* dev, uint32_t* handle, irq_vector_id vector);
obos_status DrvS_MaskIRQPin(uint32_t handle, bool mask);
// Note: Overwrites irq->irqChecker as well as irq->irqCheckerUserdata.
// It also overwrites the IRQ move callback.
typedef struct pci_irq_handle
{
    union {
        uint32_t arch_handle; // from DrvS_RegisterIRQPin
        uintptr_t msix_entry; // a pointer to the MSI-X table entry.
    } un;
    // If zero, assume arch_handle is valid.
    // If non-zero, check msix_entry.
    // If zero, this is an MSI-device, otherwise the device utilizes MSI-X.
    uint8_t msi_capability; // the offset of the MSI capability
    pci_device_location dev;
    uintptr_t msix_pending_entry;
} pci_irq_handle;
OBOS_EXPORT obos_status Drv_RegisterPCIIrq(irq* irq, const pci_device_node* dev, pci_irq_handle* handle);
OBOS_EXPORT obos_status Drv_MaskPCIIrq(const pci_irq_handle* handle, bool mask);
#else
OBOS_WEAK obos_status DrvS_EnumeratePCI(pci_iteration_decision(*cb)(void* udata, pci_device_node device), void *cb_udata);
OBOS_WEAK obos_status DrvS_ReadPCIDeviceNode(pci_device_location loc, pci_device_node* node);
OBOS_WEAK obos_status DrvS_ReadPCIRegister(pci_device_location loc, uint8_t offset, size_t accessSize, uint64_t* val);
OBOS_WEAK obos_status DrvS_WritePCIRegister(pci_device_location loc, uint8_t offset, size_t accessSize, uint64_t val);
OBOS_WEAK size_t DrvS_GetBarSize(pci_device_location loc, uint8_t barIndex, bool is64bit, size_t* size);
OBOS_WEAK uint64_t DrvS_MSIAddressAndData(uint64_t* data, uint8_t vec, uint32_t processor, bool edgetrigger, bool deassert);
OBOS_WEAK obos_status DrvS_RegisterIRQPin(const pci_device_node* dev, uint32_t* handle, irq_vector_id vector);
OBOS_WEAK obos_status DrvS_MaskIRQPin(uint32_t handle, bool mask);
// Note: Overwrites irq->irqChecker as well as irq->irqCheckerUserdata.
// It also overwrites the IRQ move callback.
typedef struct pci_irq_handle
{
    uint8_t unused;
} pci_irq_handle;
OBOS_WEAK obos_status Drv_RegisterPCIIrq(irq* irq, const pci_device_node* dev, pci_irq_handle* handle);
OBOS_WEAK obos_status Drv_MaskPCIIrq(const pci_irq_handle* handle, bool mask);
#endif