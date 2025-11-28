/*
 * oboskrnl/driver_interface/pci.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <irq/irq.h>

#include <driver_interface/pci.h>

#include <allocators/base.h>

#include <mm/page.h>

#include <vfs/vnode.h>

#if OBOS_ARCHITECTURE_HAS_ACPI
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <uacpi/namespace.h>
#endif

#include <utils/list.h>

#if OBOS_ARCHITECTURE_HAS_PCI

LIST_GENERATE_INTERNAL(pci_device_list, pci_device, node, OBOS_EXPORT);
LIST_GENERATE_INTERNAL(pci_resource_list, pci_resource, node, OBOS_EXPORT);

pci_bus Drv_PCIBuses[256];
size_t Drv_PCIBusCount;

static void update_bar(pci_device* dev, pci_resource* resource)
{
    uint8_t bar = resource->bar->idx;

    uint64_t tmp = 0;
    DrvS_ReadPCIRegister(dev->location, 0x10+bar*4, 4, &tmp);
    if (!tmp)
        return;

    // for (volatile bool b = true; b; )
    //     asm volatile ("" : :"r"(b) :"memory");

    resource->type = PCI_RESOURCE_BAR;
    resource->owner = dev;

    uint8_t bar_flags = tmp & 0b111;
    if (bar_flags & 0b1)
        bar_flags &= ~BIT(2); // Only bits 0-1 of an IO space bar are reserved, so to prevent this from breaking, clear bit 2
    // OBOS_Debug("updating bar %d address\n", bar);
    switch (bar_flags) {
        case PCI_BAR_MASK_IOSPACE:
            resource->bar->type = PCI_BARIO;
            resource->bar->phys = tmp & ~0x3;
            break;
        case PCI_BAR_MASK_32BIT:
            resource->bar->type = PCI_BAR32;
            resource->bar->prefetchable = tmp & (1<<3);
            resource->bar->phys = tmp & ~0xf;
            break;
        case PCI_BAR_MASK_64BIT:
            resource->bar->type = PCI_BAR64;
            resource->bar->prefetchable = tmp & (1<<3);
            resource->bar->phys = tmp & ~0xf;
            DrvS_ReadPCIRegister(dev->location, 0x10+(bar+1)*4, 4, &tmp);
            resource->bar->phys |= (tmp<<32);
            break;
        default: return;
    }

    // Read the size.
    // NOTE: We need to make sure IO Space and memory decode is off before doing this.

    irql oldIrql = Core_RaiseIrql(IRQL_MASKED);

    uint64_t cmd_register = 0;
    DrvS_ReadPCIRegister(dev->location, 0x4, 4, &cmd_register);
    volatile uint64_t old_cmd_register = cmd_register;
    cmd_register &= ~0b11;
    DrvS_WritePCIRegister(dev->location, 0x4, 4, cmd_register);

    switch (bar_flags)
    {
        case PCI_BAR_MASK_IOSPACE:
        case PCI_BAR_MASK_32BIT:
        {
            // Write all ones to the BAR.
            DrvS_WritePCIRegister(dev->location, 0x10+bar*4, 4, 0xffffffff);

            // Decode size.
            uint32_t sz = 0;
            uint64_t tmp2 = 0;
            DrvS_ReadPCIRegister(dev->location, 0x10+bar*4, 4, &tmp2);
            sz = (uint32_t)tmp2;
            sz = (~sz & 0xfffffff0);
            resource->bar->size = sz;
            break;
        }
        case PCI_BAR_MASK_64BIT:
        {
            // Write all ones to the BAR.
            DrvS_WritePCIRegister(dev->location, 0x10+bar*4, 4, 0xffffffff);
            DrvS_WritePCIRegister(dev->location, 0x10+(bar+1)*4, 4, 0xffffffff);

            // Decode size.
            uint64_t sz = 0;
            uint64_t tmp2 = 0;
            DrvS_ReadPCIRegister(dev->location, 0x10+bar*4, 4, &tmp2);
            sz = (uint32_t)tmp2;
            DrvS_ReadPCIRegister(dev->location, 0x10+(bar+1)*4, 4, &tmp2);
            sz |= (tmp2<<32);
            sz = (~sz & 0xfffffffffffffff0) + 1;
            resource->bar->size = sz;

            DrvS_WritePCIRegister(dev->location, 0x10+(bar+1)*4, 4, resource->bar->phys >> 32);
            break;
        }
    }

    Core_LowerIrql(oldIrql);

    // Write back old values.
    DrvS_WritePCIRegister(dev->location, 0x10+bar*4, 4, resource->bar->phys & 0xffffffff);
    DrvS_WritePCIRegister(dev->location, 0x4, 4, old_cmd_register);
}

static void initialize_bar_resources(pci_device* dev)
{
    for (uint8_t bar = 0; bar < 6; bar++)
    {
        uint64_t tmp = 0;
        DrvS_ReadPCIRegister(dev->location, 0x10+bar*4, 4, &tmp);
        if (!tmp)
            continue; // No bar here :(

        pci_resource* resource = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(pci_resource), nullptr);

        resource->owner = dev;

        resource->bar = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(pci_bar), nullptr);
        resource->bar->idx = bar;

        update_bar(dev, resource);

        const char* bar_type_str = nullptr;
        switch (resource->bar->type) {
            case PCI_BAR32:
                if (resource->bar->prefetchable)
                    bar_type_str = "32-bit, prefetchable";
                else
                    bar_type_str = "32-bit";
                break;
            case PCI_BAR64:
                if (resource->bar->prefetchable)
                    bar_type_str = "64-bit, prefetchable";
                else
                    bar_type_str = "64-bit";
                break;
            case PCI_BARIO:
                bar_type_str = "I/O";
                break;
            default: break;
        }

        OBOS_Debug("PCI: %02x:%02x:%02x: Initialized %s BAR (BAR %d). BAR ranges from 0x%p-0x%p\n",
            dev->location.bus,dev->location.slot,dev->location.function,
            bar_type_str,
            resource->bar->idx,
            resource->bar->phys, resource->bar->phys+resource->bar->size);

        if (resource->bar->type == PCI_BAR64)
            bar++;

        LIST_APPEND(pci_resource_list, &dev->resources, resource);
    }
}

static void update_cap(pci_device* dev, pci_resource* resource)
{
    uint64_t tmp = 0;

    DrvS_ReadPCIRegister(dev->location, resource->cap->offset, 4, &tmp);
    uint16_t cap_hdr = (uint16_t)(tmp&0xffff);
    resource->cap->id = cap_hdr & 0xff;
}

static void initialize_capability_resources(pci_device* dev)
{
    uint64_t status_register = 0;
    DrvS_ReadPCIRegister(dev->location, 0x4, 4, &status_register);

    if (~status_register & BIT(16+4 /* status.cap_list */))
        return; // No cap list.

    // Register each capability.

    // Capability header:
    // Bits 8-15: Next Pointer
    // Bits  0-7: Capability ID

    uint64_t tmp = 0;
    DrvS_ReadPCIRegister(dev->location, 0x34, 4, &tmp);

    pci_capability* tail = nullptr;

    uint8_t visited_offsets[256/8];
    memzero(visited_offsets, sizeof(visited_offsets));
#define set_visited_offset(off) (visited_offsets[(off)/8] |= BIT((off)%8))
#define visited_offset(off) !!(visited_offsets[(off)/8] & BIT((off)%8))

    for (uint8_t offset = tmp & 0xff; offset; )
    {
        uint8_t next = 0;
        uint8_t cap_id = 0;

        // works around certain PCs that have something like:
        // a->b->c->b
        if (visited_offset(offset))
            break; 
        set_visited_offset(offset);

        DrvS_ReadPCIRegister(dev->location, offset, 4, &tmp);
        uint16_t cap_hdr = (uint16_t)(tmp&0xffff);
        next = (cap_hdr>>8) & 0xff;
        cap_id = cap_hdr & 0xff;

        if (!cap_id)
            goto done; // don't register null capabilities, they are useless.

        pci_resource* resource = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(pci_resource), nullptr);
        resource->type = PCI_RESOURCE_CAPABILITY;

        resource->cap = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(pci_capability), nullptr);
        resource->cap->id = cap_id;
        resource->cap->offset = offset;

        if (tail)
            tail->next_cap = resource->cap;
        if (!tail)
            dev->first_capability = resource->cap;
        tail = resource->cap;

        resource->owner = dev;
        LIST_APPEND(pci_resource_list, &dev->resources, resource);

        static const char* const cap_ids[0x16] = {
            "Null",
            "PCI Power Management Interface",
            "AGP",
            "VPD",
            "Slot identification",
            "MSI",
            "CompactPCI Hot Swap",
            "PCI-X",
            "HyperTransport",
            "Vendor Specific",
            "Debug Port",
            "CompactPCI central resource control",
            "PCI Hot-Plug",
            "PCI Bridge Subsystem Vendor ID",
            "AGP 8x",
            "Secure Device",
            "PCIe",
            "MSI-X",
            "SATA Data/Index Configuration",
            "Advanced Features",
            "Enhanced Allocation",
            "Flattening Portal Bridge",
        };

        OBOS_Debug("PCI: %02x:%02x:%02x: Found %s Capability.\n",
            dev->location.bus,dev->location.slot,dev->location.function,
            cap_id > 0x15 ? "Unknown" : cap_ids[cap_id]
        );

        done:
        offset = next;
    }
#undef visited_offset
#undef set_visited_offset
}

static void initialize_irq_resources(pci_device* dev)
{
    // First check if there is an interrupt pin or
    // interrupt line in the header.

    uint64_t reg15 = 0;
    DrvS_ReadPCIRegister(dev->location, 0xf*4, 4, &reg15);

    if (!(reg15 & 0xff00) && ((reg15 & 0xff) == 0xff))
        goto check_msi;
    else
        goto done;

    check_msi:
    bool found = false;
    for (pci_capability* curr = dev->first_capability; curr; )
    {
        if  (curr->id == 0x05 || curr->id == 0x11)
        {
            found = true;
            break;
        }

        curr = curr->next_cap;
    }
    if (!found)
        return; // No IRQ, return early.

    done:
    (void)0;

    pci_resource* resource = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(pci_resource), nullptr);
    resource->type = PCI_RESOURCE_IRQ;

    resource->owner = dev;
    resource->irq = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(pci_irq_handle), nullptr);

    resource->owner = dev;
    LIST_APPEND(pci_resource_list, &dev->resources, resource);

    OBOS_Debug("PCI: %02x:%02x:%02x: Device has IRQ capabilities.\n",
        dev->location.bus,dev->location.slot,dev->location.function
    );
}

static void initialize_cmd_register_resource(pci_device* dev)
{
    pci_resource* resource = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(pci_resource), nullptr);
    resource->type = PCI_RESOURCE_CMD_REGISTER;
    uint64_t cmd_register = 0;
    DrvS_ReadPCIRegister(dev->location, 0x4, 4, &cmd_register);
    resource->cmd_register = cmd_register & 0xffff;

    LIST_APPEND(pci_resource_list, &dev->resources, resource);
    resource->owner = dev;
    dev->resource_cmd_register = resource;
}

static pci_iteration_decision bridge_cb(void* udata, pci_device_location loc);

static pci_iteration_decision init_bus_cb(void* udata, pci_device_location loc)
{
    uint64_t tmp = 0;
    DrvS_ReadPCIRegister(loc, 0xc, 4, &tmp);
    if (((tmp >> 16) & 0x7f) != 0)
    {
        if (((tmp >> 16) & 0x7f) == 1 && (((pci_bus*)udata)->busNumber != 0))
            bridge_cb((void*)__func__, loc);
        return PCI_ITERATION_DECISION_CONTINUE;
    }

    pci_bus* bus = udata;
    pci_device* dev = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(pci_device), nullptr);

    // Location info
    dev->owner = bus;
    dev->location = loc;

    // Initialize the HID.
    DrvS_ReadPCIRegister(loc, 8, 4, &tmp);
    uint8_t classCode = (tmp >> 24) & 0xff;
    uint8_t subclass = (tmp >> 16) & 0xff;
    uint8_t progIF = (tmp >> 8) & 0xff;
    DrvS_ReadPCIRegister(loc, 0, 4, &tmp);
    uint16_t deviceId = tmp >> 16;
    uint16_t vendorId = tmp & 0xffff;
    dev->hid.indiv.deviceId = deviceId;
    dev->hid.indiv.vendorId = vendorId;
    dev->hid.indiv.classCode = classCode;
    dev->hid.indiv.subClass = subclass;
    dev->hid.indiv.progIf = progIF;

    OBOS_Debug("PCI: %02x:%02x:%02x: Device HID: %02x:%02x:%02x, Vendor ID: 0x%04x, Device ID: 0x%04x\n",
        dev->location.bus,dev->location.slot,dev->location.function,
        classCode, subclass, progIF,
        vendorId, deviceId
    );

    // Initialize resources.
    // NOTE: The order of these matters.
    initialize_cmd_register_resource(dev);
    initialize_bar_resources(dev);
    initialize_capability_resources(dev);
    // DEPENDS: Capability resources.
    initialize_irq_resources(dev);

    LIST_APPEND(pci_device_list, &bus->devices, dev);

    if (OBOS_GetLogLevel() <= LOG_LEVEL_DEBUG)
        printf("\n");

    return PCI_ITERATION_DECISION_CONTINUE;
}
// Only initializes Bus 0.
// CAN and SHOULD be called before uACPI UACPI_INIT_LEVEL_NAMESPACE_LOADED
obos_status Drv_EarlyPCIInitialize()
{
    OBOS_ASSERT (uacpi_get_current_init_level() < UACPI_INIT_LEVEL_NAMESPACE_LOADED);

    Drv_PCIBusCount = 1;
    Drv_PCIBuses[0].busNumber = 0;
    // acpiNode is saved for later (Drv_PCIInitialize)

    return DrvS_EnumeratePCI(0, init_bus_cb, &Drv_PCIBuses[0]);
}

static uacpi_iteration_decision acpi_bus_cb(void* udata, uacpi_namespace_node* node, uint32_t max_depth)
{
    OBOS_UNUSED(max_depth);

    uint64_t segment = 0, bus_number = 0;
    uacpi_eval_simple_integer(node, "_SEG", &segment);
    uacpi_eval_simple_integer(node, "_BBN", &bus_number);

    // TODO: PCIe
    if (segment != 0)
        return UACPI_ITERATION_DECISION_CONTINUE;

    if (bus_number == 0)
    {
        Drv_PCIBuses[0].acpiNode = node;
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    OBOS_Debug("%s: Initializing bus %d (from ACPI)\n\n", (const char*)udata, bus_number & 0xff);

    pci_bus* bus = &Drv_PCIBuses[Drv_PCIBusCount++];
    bus->busNumber = bus_number & 0xff;
    bus->acpiNode = node;
    DrvS_EnumeratePCI(bus->busNumber, init_bus_cb, bus);

    return UACPI_ITERATION_DECISION_CONTINUE;
}

static pci_iteration_decision bridge_cb(void* udata, pci_device_location loc)
{
    uint64_t tmp = 0;
    DrvS_ReadPCIRegister(loc, 0xc, 4, &tmp);
    // OBOS_Debug("PCI: %02x:%02x:%02x: hdr type: 0x%x. register data: 0x%08x\n",
    //     loc.bus,loc.slot,loc.function,
    //     ((tmp >> 16) & 0x7f), tmp
    // );
    if (((tmp >> 16) & 0x7f) != 0x1 /* PCI->PCI Bridge */)
        return PCI_ITERATION_DECISION_CONTINUE;

    // wooo
    // we found a pci->pci bridge.

    DrvS_ReadPCIRegister(loc, 0x18, 4, &tmp);

    uint8_t secondary_bus = (tmp >> 8) & 0xff;

    OBOS_Debug("%s: Initializing bus %d (from PCI->PCI Bridge)\n\n", (const char*)udata, secondary_bus);
    pci_bus* bus = &Drv_PCIBuses[Drv_PCIBusCount++];
    bus->busNumber = secondary_bus & 0xff;
    bus->acpiNode = nullptr;
    DrvS_EnumeratePCI(bus->busNumber, init_bus_cb, bus);

    return PCI_ITERATION_DECISION_CONTINUE;
}

// Should be called after UACPI_INIT_LEVEL_NAMESPACE_LOADED.
obos_status Drv_PCIInitialize()
{
    OBOS_ASSERT (uacpi_get_current_init_level() >= UACPI_INIT_LEVEL_NAMESPACE_LOADED);

    // If PCI Device 00:00.0 is multi-function, then the other functions are the bus numbers.
    uint64_t tmp = 0;
    DrvS_ReadPCIRegister((pci_device_location){0,0,0}, 0xC, 4, &tmp);

    uint8_t hdr_type = (tmp >> 24) & 0xff;
    if (hdr_type & BIT(7))
    {
        // Multi-function device, enumerate the other functions.
        pci_device_location loc = {};
        for (loc.function = 1; loc.function < 8; loc.function++)
        {
            DrvS_ReadPCIRegister(loc, 0x0, 4, &tmp);
            if ((tmp & 0xffff) != 0xffff)
            {
                OBOS_Debug("%s: Initializing bus %d (from multi-function host bridge)\n\n", __func__, loc.function);
                pci_bus* bus = &Drv_PCIBuses[Drv_PCIBusCount++];
                bus->busNumber = loc.function;
                bus->acpiNode = nullptr;
                DrvS_EnumeratePCI(bus->busNumber, init_bus_cb, bus);
            }
        }
    }

    // Look for PCI->PCI Bridges.
    DrvS_EnumeratePCI(0, bridge_cb, (void*)__func__);

    // Look for devices NOT directly discoverable with pci.
    static const char* hids[] = {
        "PNP0A03", // PCI bus
        "PNP0A08", // PCIe bus
        nullptr,
    };
    uacpi_find_devices_at(uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB), hids, acpi_bus_cb, (void*)__func__);

    // for (volatile bool b = true; b; )
    //     asm volatile ("" : :"r"(b) :"memory");

    return OBOS_STATUS_SUCCESS;
}

static obos_status writeback_bar(pci_device* device, pci_bar* bar)
{
    // Disable memory decode and IO Space access.
    volatile uint64_t old_cmd_register = device->resource_cmd_register->cmd_register;
    if (old_cmd_register & 0b11)
    {
        uint64_t cmd_register = 0;
        DrvS_ReadPCIRegister(device->location, 0x4, 4, &cmd_register);
        cmd_register &= ~0b11;
        DrvS_WritePCIRegister(device->location, 0x4, 4, cmd_register);
    }

    obos_status status = OBOS_STATUS_SUCCESS;
    switch (bar->type) {
        case PCI_BAR32:
            status = DrvS_WritePCIRegister(device->location, 0x10+bar->idx*4, 0x4, bar->phys & 0xffffffff);
            break;
        case PCI_BAR64:
            status = DrvS_WritePCIRegister(device->location, 0x10+bar->idx*4, 0x4, bar->phys & 0xffffffff);
            status = DrvS_WritePCIRegister(device->location, 0x10+bar->idx*4+4, 0x4, bar->phys >> 32);
            break;
        case PCI_BARIO:
            status = DrvS_WritePCIRegister(device->location, 0x10+bar->idx*4, 4, bar->phys & 0xffffffff);
            break;
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }

    if (old_cmd_register & 0b11)
    {
        // Writeback the old cmd register value.
        DrvS_WritePCIRegister(device->location, 0x4, 4, old_cmd_register);
    }
    return status;
}

// Writes a resource to the appropriate place in the PCI config space.
obos_status Drv_PCISetResource(const pci_resource* resource)
{
    if (!resource)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!resource->owner)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!resource->owner->owner)
        return OBOS_STATUS_INVALID_ARGUMENT;

    obos_status status = OBOS_STATUS_SUCCESS;
    switch (resource->type) {
        case PCI_RESOURCE_BAR:
            status = writeback_bar(resource->owner, resource->bar);
            break;
        case PCI_RESOURCE_CMD_REGISTER:
            status = DrvS_WritePCIRegister(resource->owner->location, 0x4, 4, resource->cmd_register);
            break;
        case PCI_RESOURCE_CAPABILITY:
            status = OBOS_STATUS_INVALID_OPERATION;
            break;
        case PCI_RESOURCE_IRQ:
            status = Drv_UpdatePCIIrq(resource->irq->irq, resource->owner, resource->irq);
            break;
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }

    return status;
}
// Gets a resource from the appropriate place in the PCI config space.
obos_status Drv_PCIUpdateResource(pci_resource* resource)
{
    if (!resource)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!resource->owner)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!resource->owner->owner)
        return OBOS_STATUS_INVALID_ARGUMENT;

    obos_status status = OBOS_STATUS_SUCCESS;
    switch (resource->type) {
        case PCI_RESOURCE_BAR:
            update_bar(resource->owner, resource);
            break;
        case PCI_RESOURCE_CMD_REGISTER:
        {
            uint64_t tmp = 0;
            status = DrvS_ReadPCIRegister(resource->owner->location, 0x4, 4, &tmp);
            resource->cmd_register = tmp;
            break;
        }
        case PCI_RESOURCE_CAPABILITY:
            update_cap(resource->owner, resource);
            break;
        case PCI_RESOURCE_IRQ:
            status = Drv_UpdatePCIIrq(resource->irq->irq, resource->owner, resource->irq);
            break;
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }

    return status;
}

#else
#include <error.h>
obos_status Drv_EarlyPCIInitialize()
{
    return OBOS_STATUS_UNIMPLEMENTED;
}
__attribute__((alias("Drv_EarlyPCIInitialize"))) obos_status Drv_PCIInitialize();
#endif

