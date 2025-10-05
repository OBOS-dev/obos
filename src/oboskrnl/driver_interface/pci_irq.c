/*
 * oboskrnl/driver_interface/pci_irq.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <irq/irq.h>

#include <driver_interface/pci.h>

#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/page.h>

#include <scheduler/cpu_local.h>

#include <utils/tree.h>

#if OBOS_ARCHITECTURE_HAS_PCI
static void* map_registers(uintptr_t phys, size_t size, bool uc)
{
    size_t phys_page_offset = (phys % OBOS_PAGE_SIZE);
    phys -= phys_page_offset;
    size = size + (OBOS_PAGE_SIZE - (size % OBOS_PAGE_SIZE));
    size += phys_page_offset;
    void* virt = Mm_VirtualMemoryAlloc(
        &Mm_KernelContext, 
        nullptr, size,
        uc ? OBOS_PROTECTION_CACHE_DISABLE : 0, VMA_FLAGS_NON_PAGED,
        nullptr, 
        nullptr);
    for (uintptr_t offset = 0; offset < size; offset += OBOS_PAGE_SIZE)
    {
        page_info page = {.virt=offset+(uintptr_t)virt};
        MmS_QueryPageInfo(Mm_KernelContext.pt, page.virt, &page, &phys);
        page.prot.uc = uc;
        MmS_SetPageMapping(Mm_KernelContext.pt, &page, phys + offset, false);
    }
    return virt+phys_page_offset;
}
static void pci_irq_move_callback(struct irq* i, struct irq_vector* from, struct irq_vector* to, void* userdata)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(from);
    pci_irq_handle* hnd = (pci_irq_handle*)userdata;
    if (!hnd->msi_capability)
    {
        DrvS_RegisterIRQPin(&hnd->dev->location, &hnd->un.arch_handle, to->id);
        return;
    }
    uint64_t msi_data = 0;
    uint64_t msi_address = DrvS_MSIAddressAndData(&msi_data, 0, 0, true, false);
    if (hnd->un.msix_entry)
    {
        uint32_t* entry = (uint32_t*)hnd->un.msix_entry;
        entry[0] = msi_address & UINT32_MAX;
        entry[1] = msi_address >> 32;
        entry[2] = msi_data;
        return;
    }
    uint64_t header = 0; // 32-bit
    DrvS_ReadPCIRegister(hnd->dev->location, hnd->msi_capability->offset+0, 4, &header);
    uint8_t curr = 4;
    DrvS_WritePCIRegister(hnd->dev->location, hnd->msi_capability->offset+curr, 4, msi_address & UINT32_MAX);
    if (header & BIT(16+7))
    {
        curr += 4;
        DrvS_WritePCIRegister(hnd->dev->location, hnd->msi_capability->offset + curr, 4, msi_address >> 32);
    }
    curr += 4;
    DrvS_WritePCIRegister(hnd->dev->location, hnd->msi_capability->offset+ curr, 4, msi_data & UINT16_MAX);
    // Write back the header.
    DrvS_WritePCIRegister(hnd->dev->location, hnd->msi_capability->offset+0, 4, header);
}
obos_status Drv_UpdatePCIIrq(irq* irq, pci_device* dev, pci_irq_handle* handle)
{
    if (!irq || !dev || !handle)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irq->irqChecker = nullptr;
    irq->moveCallback = pci_irq_move_callback;
    irq->irqMoveCallbackUserdata = handle;
    handle->dev = dev;

    bool has_msix = false;
    bool has_msi = false;
    uint8_t msix_offset = 0;
    uint8_t msi_offset = 0;

    // uint64_t pci_status = 0;
    // DrvS_ReadPCIRegister(*dev, 1*4+2, 2, &pci_status);
    // if (!(pci_status & BIT(4)))
    //     goto fallback;

    if (!handle->initialized)
    {
        // Uninitialized object.
        // Look for a msi capability.
        for (pci_capability* curr = dev->first_capability; curr; )
        {
            switch (curr->id)
            {
                case 0x05:
                    has_msi = true;
                    msi_offset = curr->offset;
                    handle->msi_capability = curr;
                    OBOS_Debug("Found MSI capability at 0x%02x.\n", msi_offset);
                    break;
                case 0x11:
                    // has_msix = true;
                    msix_offset = curr->offset;
                    handle->msi_capability = curr;
                    OBOS_Debug("Found MSI-X capability at 0x%02x.\n", msix_offset);
                    break;
                default:
                    break;
            }

            if (has_msix)
                break;
            curr = curr->next_cap;
        }
        handle->initialized = true;
    }
    else
    {
        if (handle->msi_capability)
        {
            if (handle->un.msix_entry)
            {
                // has_msix = true;
                msix_offset = handle->msi_capability->offset;
            }
            else
            {
                has_msi = true;
                msi_offset = handle->msi_capability->offset;
            }
        }
    }
    if (!has_msi && !has_msix)
        goto fallback;
    
    cpu_local* target_cpu = nullptr;
    for (int i = Core_CpuCount-1; i >= 0; i--)
        if (!target_cpu || Core_CpuInfo[i].nMSIRoutedIRQs < target_cpu->nMSIRoutedIRQs)
            target_cpu = &Core_CpuInfo[i];
    
    uint64_t msi_data = 0;
    uint64_t msi_address = DrvS_MSIAddressAndData(&msi_data, irq->vector->id, target_cpu->id, true, false);
    if (has_msix)
    {
        // Prefer MSI-X over MSI.
        uint64_t header = 0; // 32-bit
        DrvS_ReadPCIRegister(dev->location, msix_offset+0, 4, &header);
        header |= BIT(31) /* Enable */;
        uint64_t bar_info = 0; // 32-bit register
        DrvS_ReadPCIRegister(dev->location, 4+msix_offset, 4, &bar_info);
        uint8_t bar_index = bar_info & 0x7;
        uint64_t bar = 0;
        DrvS_ReadPCIRegister(dev->location, (bar_index+4)*4, 4, &bar);
        if (((bar >> 1) & 0b11) == 0x2)
            DrvS_ReadPCIRegister(dev->location, (bar_index+5)*4, 4, (uint64_t*)(((uint32_t*)&bar) + 1));
        bar &= ~0xf;
        uint32_t bar_offset = bar_info & ~0x7;
        handle->un.msix_entry = (uintptr_t)map_registers(bar+bar_offset, OBOS_PAGE_SIZE, true);
        bar_info = 0; // 32-bit register
        DrvS_ReadPCIRegister(dev->location, 8+msix_offset, 4, &bar_info);
        bar_index = bar_info & 0x7;
        bar = 0;
        DrvS_ReadPCIRegister(dev->location, (bar_index+4)*4, 4, &bar);
        if (((bar >> 1) & 0b11) == 0x2)
            DrvS_ReadPCIRegister(dev->location, (bar_index+5)*4, 4, (uint64_t*)(((uint32_t*)&bar) + 1));
        bar &= ~0xf;
        bar_offset = bar_info & ~0x7;
        handle->msix_pending_entry = (uintptr_t)map_registers(bar+bar_offset, OBOS_PAGE_SIZE, true);
        uint32_t* entry = (uint32_t*)handle->un.msix_entry;
        entry[0] = msi_address & UINT32_MAX;
        entry[1] = msi_address >> 32;
        entry[2] = msi_data;
        if (handle->masked)
            entry[3] |= BIT(0); // masked
        else
            entry[3] &= ~BIT(0); // unmasked
        // Write back the header.
        DrvS_WritePCIRegister(dev->location, msix_offset+0, 4, header);
        return OBOS_STATUS_SUCCESS;
    }
    if (has_msi)
    {
        // printf("choosing MSI for device %02x:%02x:%02x\n",
        //     dev->location.bus,dev->location.slot,dev->location.function
        // );
        // Fallback to MSI.
        uint64_t header = 0; // 32-bit
        DrvS_ReadPCIRegister(dev->location, msi_offset+0, 4, &header);
        OBOS_Debug("header=0x%x\n", header);
        handle->un.msix_entry = 0;
        handle->msix_pending_entry = 0;
        header |= BIT(16) /* Enable */;
        uint8_t curr = 4;
        DrvS_WritePCIRegister(dev->location, msi_offset+curr, 4, msi_address & UINT32_MAX);
        if (header & BIT(16+7))
        {
            curr += 4;
            DrvS_WritePCIRegister(dev->location, msi_offset + curr, 4, msi_address >> 32);
        }
        curr += 4;
        DrvS_WritePCIRegister(dev->location, msi_offset + curr, 4, msi_data & UINT16_MAX);
        // Write back the header.
        DrvS_WritePCIRegister(dev->location, msi_offset+0, 4, header);
        if (header & BIT(8+16))
        {
            // Set Mask.
            DrvS_WritePCIRegister(handle->dev->location, msi_offset + 0x10, 4, handle->masked);
        }
        return OBOS_STATUS_SUCCESS;
    }
    fallback:
    // printf("choosing legacy IRQs for device %02x:%02x:%02x\n",
    //     dev->location.bus,dev->location.slot,dev->location.function
    // );
    // Use legacy IRQs.
    if (DrvS_CheckIrqCallbackIrqPin)
    {
        irq->irqChecker = DrvS_CheckIrqCallbackIrqPin;
        irq->irqCheckerUserdata = handle;
    }
    obos_status stat = DrvS_RegisterIRQPin(&dev->location, &handle->un.arch_handle, irq->vector->id);
    if (obos_is_error(stat))
        return stat;
    return DrvS_MaskIRQPin(handle->un.arch_handle, handle->masked);
}
#endif
