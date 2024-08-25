/*
 * oboskrnl/driver_interface/pci_irq.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <irq/irq.h>

#include <driver_interface/pci.h>

#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/page.h>

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
    page what = {.addr=(uintptr_t)virt};
    for (uintptr_t offset = 0; offset < size; offset += OBOS_PAGE_SIZE)
    {
        what.addr = (uintptr_t)virt + offset;
        page* page = RB_FIND(page_tree, &Mm_KernelContext.pages, &what);
        page->prot.uc = true;
        MmS_SetPageMapping(Mm_KernelContext.pt, page, phys + offset);
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
        pci_device_node node = {};
        DrvS_ReadPCIDeviceNode(hnd->dev, &node);
        DrvS_RegisterIRQPin(&node, &hnd->un.arch_handle, to->id);
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
    DrvS_ReadPCIRegister(hnd->dev, hnd->msi_capability+0, 4, &header);
    uint8_t curr = 4;
    DrvS_WritePCIRegister(hnd->dev, hnd->msi_capability+curr, 4, msi_address & UINT32_MAX);
    if (header & BIT(16+7))
    {
        curr += 4;
        DrvS_WritePCIRegister(hnd->dev, hnd->msi_capability + curr, 4, msi_address >> 32);
    }
    curr += 4;
    DrvS_WritePCIRegister(hnd->dev, hnd->msi_capability + curr, 4, msi_data & UINT16_MAX);
    // Write back the header.
    DrvS_WritePCIRegister(hnd->dev, hnd->msi_capability+0, 4, header);
}
obos_status Drv_RegisterPCIIrq(irq* irq, const pci_device_node* dev, pci_irq_handle* handle)
{
    if (!irq || !dev || !handle)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irq->irqChecker = nullptr;
    irq->moveCallback = pci_irq_move_callback;
    irq->irqMoveCallbackUserdata = handle;
    handle->dev = dev->info;
    handle->msi_capability = 0;
    handle->un.msix_entry = 0;
    uint64_t pci_status = 0;
    bool has_msix = false;
    bool has_msi = false;
    uint8_t msix_offset = 0;
    uint8_t msi_offset = 0;
    DrvS_ReadPCIRegister(dev->info, 1*4+2, 2, &pci_status);
    if (!(pci_status & BIT(4)))
        goto fallback;
    // Look for an MSI(-X capability).
    uint64_t cap_header = 0; // 16-bit
    uint64_t offset = 0;
    DrvS_ReadPCIRegister(dev->info, 0x34, 1, &offset);
    DrvS_ReadPCIRegister(dev->info, offset, 2, &cap_header);
    OBOS_Debug("First capability at 0x%02x.\n", offset);
    size_t nEntries = 0;
    const size_t threshold = 10;
    for (; offset != 0 && nEntries < threshold; offset = ((cap_header >> 8) & 0xff), nEntries++)
    {
        cap_header = 0;
        DrvS_ReadPCIRegister(dev->info, offset, 2, &cap_header);
        bool abort = false;
        switch (cap_header & 0xff)
        {
            case 0x05:
                has_msi = true;
                msi_offset = offset;
                OBOS_Debug("Found MSI capability at 0x%02x.\n", msi_offset);
                break;
            case 0x11:
                has_msix = true;
                msix_offset = offset;
                OBOS_Debug("Found MSI-X capability at 0x%02x.\n", msix_offset);
                break;
            case 0x00:
                abort = true;
                break;
            default:
                break;
        }
        if (has_msix || abort)
            break; // If we have MSI-X, break.
    }
    if (!has_msi && !has_msix)
        goto fallback;
    uint64_t msi_data = 0;
    uint64_t msi_address = DrvS_MSIAddressAndData(&msi_data, irq->vector->id, 0, true, false);
    if (has_msix)
    {
        // Prefer MSI-X over MSI.
        uint64_t header = 0; // 32-bit
        DrvS_ReadPCIRegister(dev->info, msix_offset+0, 4, &header);
        header |= BIT(31) /* Enable */;
        uint64_t bar_info = 0; // 32-bit register
        DrvS_ReadPCIRegister(dev->info, 4+msix_offset, 4, &bar_info);
        uint8_t bar_index = bar_info & 0x7;
        uint64_t bar = 0;
        DrvS_ReadPCIRegister(dev->info, (bar_index+4)*4, 4, &bar);
        if (((bar >> 1) & 0b11) == 0x2)
            DrvS_ReadPCIRegister(dev->info, (bar_index+5)*4, 4, (uint64_t*)(((uint32_t*)&bar) + 1));
        bar &= ~0xf;
        uint32_t bar_offset = bar_info & ~0x7;
        handle->msi_capability = msix_offset;
        handle->un.msix_entry = (uintptr_t)map_registers(bar+bar_offset, OBOS_PAGE_SIZE, true);
        bar_info = 0; // 32-bit register
        DrvS_ReadPCIRegister(dev->info, 8+msix_offset, 4, &bar_info);
        bar_index = bar_info & 0x7;
        bar = 0;
        DrvS_ReadPCIRegister(dev->info, (bar_index+4)*4, 4, &bar);
        if (((bar >> 1) & 0b11) == 0x2)
            DrvS_ReadPCIRegister(dev->info, (bar_index+5)*4, 4, (uint64_t*)(((uint32_t*)&bar) + 1));
        bar &= ~0xf;
        bar_offset = bar_info & ~0x7;
        handle->msix_pending_entry = (uintptr_t)map_registers(bar+bar_offset, OBOS_PAGE_SIZE, true);
        uint32_t* entry = (uint32_t*)handle->un.msix_entry;
        entry[0] = msi_address & UINT32_MAX;
        entry[1] = msi_address >> 32;
        entry[2] = msi_data;
        entry[3] |= BIT(0); // masked
        // Write back the header.
        DrvS_WritePCIRegister(dev->info, msix_offset+0, 4, header);
        return OBOS_STATUS_SUCCESS;
    }
    if (has_msi)
    {
        // Fallback to MSI.
        uint64_t header = 0; // 32-bit
        DrvS_ReadPCIRegister(dev->info, msi_offset+0, 4, &header);
        handle->msi_capability = msi_offset;
        handle->un.msix_entry = 0;
        handle->msix_pending_entry = 0;
        header |= BIT(16) /* Enable */;
        uint8_t curr = 4;
        DrvS_WritePCIRegister(dev->info, msi_offset+curr, 4, msi_address & UINT32_MAX);
        if (header & BIT(16+7))
        {
            curr += 4;
            DrvS_WritePCIRegister(dev->info, msi_offset + curr, 4, msi_address >> 32);
        }
        curr += 4;
        DrvS_WritePCIRegister(dev->info, msi_offset + curr, 4, msi_data & UINT16_MAX);
        // Write back the header.
        DrvS_WritePCIRegister(dev->info, msi_offset+0, 4, header);
        Drv_MaskPCIIrq(handle, true);
        return OBOS_STATUS_SUCCESS;
    }
    fallback:
    // Use legacy IRQs.
    irq->irqChecker = DrvS_CheckIrqCallbackIrqPin;
    irq->irqCheckerUserdata = handle;
    return DrvS_RegisterIRQPin(dev, &handle->un.arch_handle, irq->vector->id);
}
obos_status Drv_MaskPCIIrq(const pci_irq_handle* handle, bool mask)
{
    if (!handle)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!handle->msi_capability)
        return DrvS_MaskIRQPin(handle->un.arch_handle, mask);
    if (handle->un.msix_entry)
    {
        uint32_t* msix_entry = (uint32_t*)handle->un.msix_entry;
        if (mask)
            msix_entry[3] |= BIT(0);
        else
            msix_entry[3] &= ~BIT(0);
        return OBOS_STATUS_SUCCESS;
    }
    // Fallback to MSI.
    uint64_t header = 0; // 32-bit
    DrvS_ReadPCIRegister(handle->dev, handle->msi_capability+0, 4, &header);
    if (header & BIT(8+16))
    {
        // Mask IRQ.
        DrvS_WritePCIRegister(handle->dev, handle->msi_capability+0x10, 4, 1);
    }
    return OBOS_STATUS_SUCCESS;
}
#endif