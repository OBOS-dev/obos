/*
 * oboskrnl/arch/x86_64/pci.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <driver_interface/pci.h>

#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/ioapic.h>

#include <uacpi/namespace.h>
#include <uacpi/resources.h>
#include <uacpi/types.h>
#include <uacpi/utilities.h>

// Funny how these same functions for io from PCI registers have been in use since OBOS Rewrite #2
// (Branch old_code2 in OBOS-dev/obos-old)

void pciWriteByteRegister(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t data)
{
    uint32_t address;
    uint32_t lbus = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;

    address = (uint32_t)((lbus << 16) | (lslot << 11) |
        (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

    outd(0xCF8, address);
    outb(0xCFC, data);
}
void pciWriteWordRegister(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t data)
{
    uint32_t address;
    uint32_t lbus = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;

    address = (uint32_t)((lbus << 16) | (lslot << 11) |
        (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

    outd(0xCF8, address);
    outw(0xCFC, data);
}
void pciWriteDwordRegister(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t data)
{
    uint32_t address;
    uint32_t lbus = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;

    address = (uint32_t)((lbus << 16) | (lslot << 11) |
        (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

    outd(0xCF8, address);
    outd(0xCFC, data);
}
uint8_t pciReadByteRegister(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t address;
    uint32_t lbus = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;

    address = (uint32_t)((lbus << 16) | (lslot << 11) |
        (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

    outd(0xCF8, address);

    uint8_t ret = (uint16_t)((ind(0xCFC) >> ((offset & 2) * 8)) & 0xFFFFFF);
    return ret;
}
uint16_t pciReadWordRegister(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t address;
    uint32_t lbus = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;

    address = (uint32_t)((lbus << 16) | (lslot << 11) |
        (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

    outd(0xCF8, address);

    uint16_t ret = (uint16_t)((ind(0xCFC) >> ((offset & 2) * 8)) & 0xFFFF);
    return ret;
}
uint32_t pciReadDwordRegister(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t address;
    uint32_t lbus = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;

    address = (uint32_t)((lbus << 16) | (lslot << 11) |
        (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

    outd(0xCF8, address);

    return ((ind(0xCFC) >> ((offset & 2) * 8)));
}
OBOS_EXPORT obos_status DrvS_EnumeratePCI(pci_iteration_decision(*cb)(void* udata, pci_device_node device), void *cb_udata)
{
    int16_t currentSlot = 0;
    for (int16_t bus = 0; bus < 256; bus++)
    {
        for (; currentSlot < 256; currentSlot++)
        {
            for (int function = 0; function < 8; function++)
            {
                if (pciReadWordRegister(bus, currentSlot, function, 0) == 0xFFFF)
                {
                    if (!function)
                        break;
                    continue; // There is no device here.
                }
                uint32_t tmp = pciReadDwordRegister(bus, currentSlot, function, 8);
                uint8_t classCode = ((uint8_t*)&tmp)[3];
                uint8_t subclass = ((uint8_t*)&tmp)[2];
                uint8_t progIF = ((uint8_t*)&tmp)[1];
                uint16_t deviceId = pciReadDwordRegister(bus, currentSlot, function, 0) & 0xff;
                uint16_t vendorId = pciReadDwordRegister(bus, currentSlot, function, 0) >> 16;
                uint16_t int_info = pciReadDwordRegister(bus, currentSlot, function, 0xf*4);
                pci_device_node dev = {
                    .info = {
                        .bus = bus,
                        .slot = currentSlot,
                        .function = function,
                    },           
                    .device = {
                        .indiv = {
                            .classCode = classCode,
                            .subClass = subclass,
                            .progIf = progIF,
                            .vendorId = vendorId,
                            .deviceId = deviceId,
                        }
                    },
                    .bars = {
                        .arr32 = {
                            pciReadDwordRegister(bus, currentSlot, function, 4*4) & 0xfffffff0,
                            pciReadDwordRegister(bus, currentSlot, function, 5*4) & 0xfffffff0,
                            pciReadDwordRegister(bus, currentSlot, function, 6*4) & 0xfffffff0,
                            pciReadDwordRegister(bus, currentSlot, function, 7*4) & 0xfffffff0,
                            pciReadDwordRegister(bus, currentSlot, function, 8*4) & 0xfffffff0,
                            pciReadDwordRegister(bus, currentSlot, function, 9*4) & 0xfffffff0,
                        }
                    },
                    .irq = {
                        .int_line = int_info & 0xff,
                        .int_pin = int_info >> 8,
                    }
                };
                if (!cb(cb_udata, dev))
                    break;
                if(!function)
                    if ((pciReadByteRegister(bus, currentSlot, 0, 15) & 0x80) != 0x80)
                        break;
            }
        }
    }
    return OBOS_STATUS_SUCCESS;
}
OBOS_EXPORT obos_status DrvS_ReadPCIDeviceNode(pci_device_location loc, pci_device_node* node)
{
    if (pciReadWordRegister(loc.bus, loc.slot, loc.function, 0) == 0xFFFF)
        return OBOS_STATUS_NOT_FOUND; // There is no device here.
    uint8_t bus = loc.bus;
    uint8_t currentSlot = loc.slot;
    uint8_t function = loc.function;
    uint32_t tmp = pciReadDwordRegister(bus, currentSlot, function, 8);
    uint8_t classCode = ((uint8_t*)&tmp)[3];
    uint8_t subclass = ((uint8_t*)&tmp)[2];
    uint8_t progIF = ((uint8_t*)&tmp)[1];
    uint16_t deviceId = pciReadDwordRegister(bus, currentSlot, function, 0*4) & 0xff;
    uint16_t vendorId = pciReadDwordRegister(bus, currentSlot, function, 0*4) >> 16;
    uint16_t int_info = pciReadDwordRegister(bus, currentSlot, function, 0xf*4);
    pci_device_node dev = {
        .info = {
            .bus = bus,
            .slot = currentSlot,
            .function = function,
        },           
        .device = {
            .indiv = {
                .classCode = classCode,
                .subClass = subclass,
                .progIf = progIF,
                .vendorId = vendorId,
                .deviceId = deviceId,
            }
        },
        .bars = {
            .arr32 = {
                pciReadDwordRegister(bus, currentSlot, function, 4*4),
                pciReadDwordRegister(bus, currentSlot, function, 5*4),
                pciReadDwordRegister(bus, currentSlot, function, 6*4),
                pciReadDwordRegister(bus, currentSlot, function, 7*4),
                pciReadDwordRegister(bus, currentSlot, function, 8*4),
                pciReadDwordRegister(bus, currentSlot, function, 9*4),
            }
        },
        .irq = {
            .int_line = int_info & 0xff,
            .int_pin = int_info >> 8,
        }
    };
    *node = dev;
    return OBOS_STATUS_SUCCESS;
}
OBOS_EXPORT obos_status DrvS_ReadPCIRegister(pci_device_location loc, uint8_t offset, size_t accessSize, uint64_t* val)
{
    if (!val)
        return OBOS_STATUS_INVALID_ARGUMENT;
    switch (accessSize)
    {
        case 1:
            *val = pciReadByteRegister(loc.bus, loc.slot, loc.function, offset);
            break;
        case 2:
            *val = pciReadWordRegister(loc.bus, loc.slot, loc.function, offset);
            break;
        case 4:
            *val = pciReadDwordRegister(loc.bus, loc.slot, loc.function, offset);
            break;
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    return OBOS_STATUS_SUCCESS;
}
OBOS_EXPORT obos_status DrvS_WritePCIRegister(pci_device_location loc, uint8_t offset, size_t accessSize, uint64_t val)
{
    switch (accessSize)
    {
        case 1:
            pciWriteByteRegister(loc.bus, loc.slot, loc.function, offset, val);
            break;
        case 2:
            pciWriteWordRegister(loc.bus, loc.slot, loc.function, offset, val);
            break;
        case 4:
            pciWriteDwordRegister(loc.bus, loc.slot, loc.function, offset, val);
            break;
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    return OBOS_STATUS_SUCCESS;
}
OBOS_EXPORT size_t DrvS_GetBarSize(pci_device_location loc, uint8_t bar_index, bool is64bit, obos_status* status)
{
    const uint8_t bus = loc.bus;
    const uint8_t slot = loc.slot;
    const uint8_t function = loc.function;
    if (status)
        *status = OBOS_STATUS_SUCCESS;
    if (bar_index > 5+(!is64bit))
    {
        if (status)
            *status = OBOS_STATUS_INVALID_ARGUMENT;
        return (size_t)-1;
    }
    uint32_t bar = pciReadDwordRegister(bus, slot, function, (4+bar_index)*4) & 0xfffffff0;
    pciWriteDwordRegister(bus, slot, function, (4+bar_index)*4, 0xFFFFFFFF);
    size_t size = (~pciReadDwordRegister(bus, slot, function, (4+bar_index)*4) & 0xfffffff0) + 1;
    // size = ((size >> 12) + 1) << 12;
    pciWriteDwordRegister(bus, slot, function, (4+bar_index)*4, bar);
    return size;
}
uint64_t DrvS_MSIAddressAndData(uint64_t* data, irq_vector_id vec, uint32_t processor, bool edgetrigger, bool deassert)
{
     if (!data)
        return 0;
    vec += 0x20;
    // Shamelessly stolen from the osdev wiki.
    *data = (vec & 0xFF) | (edgetrigger == 1 ? 0 : (1 << 15)) | (deassert == 1 ? 0 : (1 << 14));
	return (0xFEE00000 | (processor << 12));
}
uacpi_ns_iteration_decision pci_bus_match(void *user, uacpi_namespace_node *node)
{
    uacpi_namespace_node** pNode = (uacpi_namespace_node**)user;
    *pNode = node;
    return UACPI_NS_ITERATION_DECISION_BREAK;
}
obos_status DrvS_RegisterIRQPin(const pci_device_node* dev, uint32_t* handle, irq_vector_id vector)
{
    if (!dev || !handle)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // Get PCI bus.
    uacpi_namespace_node* pciBus = nullptr;
    uacpi_find_devices("PNP0A03", pci_bus_match, &pciBus);
    uacpi_pci_routing_table *pci_routing_table = nullptr;
    uacpi_get_pci_routing_table(pciBus, &pci_routing_table);
    bool wasGoodMatch = false;
    ioapic_trigger_mode triggerMode = 0;
    bool polarity = false;
    uint32_t gsi = 0;
    for (size_t i = 0, pi = 0; i < pci_routing_table->num_entries; i++, pi++)
    {
        if (pci_routing_table->entries[i].pin != (dev->irq.int_pin - 1))
            continue;
        // OBOS_Debug("Found PIN%c.\n", (dev->irq.int_pin - 1) + 'A');
        uint16_t function = (pci_routing_table->entries[i].address & 0xffff);
        uint16_t slot = (pci_routing_table->entries[i].address >> 16);
        // OBOS_Debug("slot: 0x%04x\n", slot);
        // OBOS_Debug("function: 0x%04x\n", function);
        if (slot != dev->info.slot && (function != dev->info.function || function == 0xffff))
            continue;
        if (pci_routing_table->entries[i].source == 0)
            gsi = pci_routing_table->entries[i].index;
        else
        {
            uacpi_resources* resources = nullptr;
            uacpi_get_current_resources(pci_routing_table->entries[i].source, &resources);

            switch (resources->entries[pci_routing_table->entries[i].index].type)
            {
                case UACPI_RESOURCE_TYPE_IRQ:
                    gsi = resources->entries[pci_routing_table->entries[i].index].irq.irqs[0];
                    polarity = resources->entries[pci_routing_table->entries[i].index].irq.polarity == UACPI_POLARITY_ACTIVE_LOW ?
                        true : false;
                    triggerMode = resources->entries[pci_routing_table->entries[i].index].irq.triggering == UACPI_TRIGGERING_EDGE ?
                        TriggerModeEdgeSensitive : TriggerModeLevelSensitive;
                    break;
                case UACPI_RESOURCE_TYPE_EXTENDED_IRQ:
                    gsi = resources->entries[pci_routing_table->entries[i].index].extended_irq.irqs[0];
                    polarity = resources->entries[pci_routing_table->entries[i].index].extended_irq.polarity == UACPI_POLARITY_ACTIVE_LOW ?
                        true : false;
                    triggerMode = resources->entries[pci_routing_table->entries[i].index].extended_irq.triggering == UACPI_TRIGGERING_EDGE ?
                        TriggerModeEdgeSensitive : TriggerModeLevelSensitive;
                    break;
                default:
                    OBOS_ASSERT(false && "Invalid resource type");
                    break;
            }
            wasGoodMatch = (function != 0xffff);
            uacpi_free_resources(resources);
            if (wasGoodMatch)
                break;
        }
        break;
    }
    uacpi_free_pci_routing_table(pci_routing_table);
    Arch_IOAPICMapIRQToVector(gsi, vector+0x20, polarity, triggerMode);
    *handle = gsi;
    return OBOS_STATUS_SUCCESS;
}
obos_status DrvS_MaskIRQPin(uint32_t handle, bool mask)
{
    // handle is the GSI.
    return Arch_IOAPICMaskIRQ(handle & 0xffffffff, mask);
}