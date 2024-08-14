/*
 * oboskrnl/arch/x86_64/pci.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>

#include <driver_interface/pci.h>

#include <arch/x86_64/asm_helpers.h>

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
obos_status DrvS_EnumeratePCI(pci_iteration_decision(*cb)(void* udata, pci_device_node device), void *cb_udata)
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
                uint16_t int_info = pciReadDwordRegister(bus, currentSlot, function, 0xf);
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
                            pciReadDwordRegister(bus, currentSlot, function, 4),
                            pciReadDwordRegister(bus, currentSlot, function, 5),
                            pciReadDwordRegister(bus, currentSlot, function, 6),
                            pciReadDwordRegister(bus, currentSlot, function, 7),
                            pciReadDwordRegister(bus, currentSlot, function, 8),
                            pciReadDwordRegister(bus, currentSlot, function, 9),
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
obos_status DrvS_ReadPCIDeviceNode(pci_device_location loc, pci_device_node* node)
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
    uint16_t deviceId = pciReadDwordRegister(bus, currentSlot, function, 0) & 0xff;
    uint16_t vendorId = pciReadDwordRegister(bus, currentSlot, function, 0) >> 16;
    uint16_t int_info = pciReadDwordRegister(bus, currentSlot, function, 0xf);
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
                pciReadDwordRegister(bus, currentSlot, function, 4),
                pciReadDwordRegister(bus, currentSlot, function, 5),
                pciReadDwordRegister(bus, currentSlot, function, 6),
                pciReadDwordRegister(bus, currentSlot, function, 7),
                pciReadDwordRegister(bus, currentSlot, function, 8),
                pciReadDwordRegister(bus, currentSlot, function, 9),
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
obos_status DrvS_ReadPCIRegister(pci_device_location loc, uint8_t offset, size_t accessSize, uint64_t* val)
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
obos_status DrvS_WritePCIRegister(pci_device_location loc, uint8_t offset, size_t accessSize, uint64_t val)
{
    if (!val)
        return OBOS_STATUS_INVALID_ARGUMENT;
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