/*
 * oboskrnl/arch/x86_64/ioapic.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <error.h>

#include <allocators/base.h>

#include <arch/x86_64/ioapic.h>

#include <arch/x86_64/pmm.h>
#include <arch/x86_64/boot_info.h>

#include <uacpi/acpi.h>
#include <uacpi/uacpi.h>
#include <uacpi/tables.h>

ioapic_irq_redirection_entry* Arch_IRQRedirectionEntries;
size_t Arch_SizeofIRQRedirectionEntries;
ioapic_descriptor* Arch_IOAPICs;
size_t Arch_IOAPICCount;

#define OffsetPtr(ptr, off, t) ((t*)(((uintptr_t)(ptr)) + (off)))
#define NextMADTEntry(cur) OffsetPtr(cur, cur->length, struct acpi_entry_hdr)
// #define OffsetOfReg(reg) ((uintptr_t)&(((ioapic_registers*)nullptr)->reg))
#define OffsetOfReg(reg) offsetof(ioapic_registers, reg)

static OBOS_PAGEABLE_FUNCTION OBOS_NO_UBSAN obos_status ParseMADT()
{
	uacpi_table madt_table = {};
    uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &madt_table);
	if (!madt_table.ptr)
        return OBOS_STATUS_UNIMPLEMENTED;
    struct acpi_madt *madt = (void*)madt_table.hdr;
    void* end = OffsetPtr(madt, madt->hdr.length, void);
    
	for (struct acpi_entry_hdr* cur = OffsetPtr(madt, sizeof(*madt), struct acpi_entry_hdr); (uintptr_t)cur < (uintptr_t)end; cur = NextMADTEntry(cur))
	{
		if (cur->type == 1)
            Arch_IOAPICCount++;
		if (cur->type == 2)
        {
            if (((struct acpi_madt_interrupt_source_override*)cur)->source != 0 /* ISA */)
                continue;
            if ((((struct acpi_madt_interrupt_source_override*)cur)->flags & ACPI_MADT_POLARITY_MASK) == 0b10)
                continue;
            if (((((struct acpi_madt_interrupt_source_override*)cur)->flags >> 2) & 0b11) == 0b10)
                continue;
            Arch_SizeofIRQRedirectionEntries++;
        }
	}
    if (Arch_IOAPICCount > 16)
        return OBOS_STATUS_INTERNAL_ERROR;
    obos_status status = OBOS_STATUS_SUCCESS;
    Arch_IRQRedirectionEntries = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, Arch_SizeofIRQRedirectionEntries, sizeof(ioapic_irq_redirection_entry), &status);
	if (obos_is_error(status))
        return status;
    Arch_IOAPICs = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, Arch_IOAPICCount, sizeof(ioapic_descriptor), &status);
	if (obos_is_error(status))
        return status;
    size_t ioapic_index = 0;
    size_t ioapic_redir_index = 0;
	for (struct acpi_entry_hdr* cur = OffsetPtr(madt, sizeof(*madt), struct acpi_entry_hdr); (uintptr_t)cur < (uintptr_t)end; cur = NextMADTEntry(cur))
	{
		if (cur->type == 1)
        {
            // IOAPIC
            struct acpi_madt_ioapic* ent = (void*)cur;
            Arch_IOAPICs[ioapic_index].id = ent->id;
            Arch_IOAPICs[ioapic_index].phys = ent->address;
            Arch_IOAPICs[ioapic_index].gsi = ent->gsi_base;
            Arch_IOAPICs[ioapic_index].address = Arch_MapToHHDM(ent->address);
            Arch_IOAPICs[ioapic_index].maxRedirectionEntries = 
                ArchH_IOAPICReadRegister(Arch_IOAPICs[ioapic_index].address, OffsetOfReg(ioapicVersion.maximumRedirectionEntries)) & 0xff;
            ioapic_index++;
        }
		if (cur->type == 2)
        {
            // IOAPIC Redirection entry
            struct acpi_madt_interrupt_source_override* ent = (struct acpi_madt_interrupt_source_override*)cur;
            if (ent->source != 0 /* ISA */)
                continue;
            uint8_t polarity = ent->flags & 0b11;
            OBOS_ASSERT(polarity != 0b10);
            bool res = false;
            if (polarity == 0b1 || polarity == 0)
                res = PolarityActiveHigh;
            else if (polarity == 0b11)
                res = PolarityActiveLow;
            ioapic_trigger_mode tm = TriggerModeLevelSensitive;
            uint8_t tmFlg = (ent->flags >> 2) & 0b11;
            OBOS_ASSERT(tmFlg != 0b10);
            if (tmFlg <= 0b1)
                tm = TriggerModeEdgeSensitive;
            else if (tmFlg == 0b11)
                tm = TriggerModeLevelSensitive;
            Arch_IRQRedirectionEntries[ioapic_redir_index].globalSystemInterrupt = ent->gsi;
            Arch_IRQRedirectionEntries[ioapic_redir_index].polarity = res;
            Arch_IRQRedirectionEntries[ioapic_redir_index].tm = tm;
            ioapic_redir_index++;
        }
	}
    return OBOS_STATUS_SUCCESS;
}

OBOS_PAGEABLE_FUNCTION obos_status Arch_InitializeIOAPICs()
{
    obos_status status = ParseMADT();
    if (obos_is_error(status))
        return status;
    OBOS_ASSERT(Arch_IOAPICCount <= 16);
    // Initialize each I/O APIC.
    for (size_t i = 0; i < Arch_IOAPICCount; i++)
    {
        ioapic_descriptor* ioapic = &Arch_IOAPICs[i];
        ArchH_IOAPICWriteRegister(ioapic->address, 0, i<<24);
        for (uint32_t gsi = ioapic->gsi; gsi <= ioapic->gsi+ioapic->maxRedirectionEntries; gsi++)
            OBOS_ASSERT(obos_is_success(Arch_IOAPICMapIRQToVector(gsi, 0, PolarityActiveHigh, TriggerModeEdgeSensitive)));
    }
    return status;
}
static OBOS_PAGEABLE_FUNCTION size_t redirection_entry_index(uint32_t gsi)
{
    size_t i = 0;
    for (; i < Arch_SizeofIRQRedirectionEntries; i++)
        if (Arch_IRQRedirectionEntries[i].source == gsi)
            return i;
    return SIZE_MAX;
}
static OBOS_PAGEABLE_FUNCTION ioapic_descriptor* find_ioapic(uint32_t gsi)
{
    for (size_t i = 0; i < Arch_IOAPICCount; i++)
        if (gsi >= Arch_IOAPICs[i].gsi && gsi <= (Arch_IOAPICs[i].gsi + Arch_IOAPICs[i].maxRedirectionEntries))
            return &Arch_IOAPICs[i];
    return nullptr;
}
OBOS_PAGEABLE_FUNCTION obos_status Arch_IOAPICMaskIRQ(uint32_t gsi, bool mask)
{
    size_t redir_entry = redirection_entry_index(gsi);
    if (redir_entry != SIZE_MAX)
    {
        ioapic_irq_redirection_entry* ent = Arch_IRQRedirectionEntries + redir_entry;
        OBOS_ASSERT(ent->source == gsi);
        gsi = ent->globalSystemInterrupt;
    }
    ioapic_descriptor* ioapic = find_ioapic(gsi);
    if (!ioapic)
        return OBOS_STATUS_NOT_FOUND;
    // Gets the redirection entry.
    uint32_t ent[2];
    ent[0] = ((uint64_t)ArchH_IOAPICReadRegister(ioapic->address, OffsetOfReg(redirectionEntries[gsi-ioapic->gsi])));
    ent[1] = ((uint64_t)ArchH_IOAPICReadRegister(ioapic->address, OffsetOfReg(redirectionEntries[gsi-ioapic->gsi]) + 4));
    ioapic_redirection_entry *entry = (ioapic_redirection_entry*)&ent;
    if (!entry->vector)
        return OBOS_STATUS_UNINITIALIZED;
    entry->mask = mask;
    ArchH_IOAPICWriteRegister(ioapic->address, OffsetOfReg(redirectionEntries[gsi-ioapic->gsi]) + 4, ent[1]);
    ArchH_IOAPICWriteRegister(ioapic->address, OffsetOfReg(redirectionEntries[gsi-ioapic->gsi]),     ent[0]);
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status Arch_IOAPICMapIRQToVector(uint32_t gsi, uint8_t vector, ioapic_polarity polarity, ioapic_trigger_mode tm)
{
    if (tm < 0 || tm > 1)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (vector < 0x20 && vector != 0)
        return OBOS_STATUS_INVALID_ARGUMENT;
    size_t redir_entry = redirection_entry_index(gsi);
    if (redir_entry != SIZE_MAX)
    {
        ioapic_irq_redirection_entry* ent = Arch_IRQRedirectionEntries + redir_entry;
        OBOS_ASSERT(ent->source == gsi);
        polarity = ent->polarity;
        tm = ent->tm;
        gsi = ent->globalSystemInterrupt;
    }
    ioapic_descriptor* ioapic = find_ioapic(gsi);
    if (!ioapic)
        return OBOS_STATUS_NOT_FOUND;
    // Gets the redirection entry.
    uint32_t ent[2];
    ent[0] = ((uint64_t)ArchH_IOAPICReadRegister(ioapic->address, OffsetOfReg(redirectionEntries[gsi-ioapic->gsi])));
    ent[1] = ((uint64_t)ArchH_IOAPICReadRegister(ioapic->address, OffsetOfReg(redirectionEntries[gsi-ioapic->gsi]) + 4));
    ioapic_redirection_entry *entry = (ioapic_redirection_entry*)&ent;
    // Set vector info.
    if (vector)
    {
        entry->vector = vector;
        entry->delMod = 0b000 /* FIXED */;
        // Set polarity info.
        entry->intPol = polarity;
        entry->triggerMode = tm;
        // Set LAPIC info.
        entry->destMode = 0 /* Physical */;
        entry->destination.physical.lapicId = 0 /* BSP */;
    }
    else 
    {
        memzero(entry, sizeof(*entry));
    }
    // Mask the IRQ.
    entry->mask = true;
    ArchH_IOAPICWriteRegister(ioapic->address, OffsetOfReg(redirectionEntries[gsi-ioapic->gsi]) + 4, ent[1]);
    ArchH_IOAPICWriteRegister(ioapic->address, OffsetOfReg(redirectionEntries[gsi-ioapic->gsi]),     ent[0]);
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status Arch_IOAPICGSIUsed(uint32_t gsi)
{
    size_t redir_entry = redirection_entry_index(gsi);
    if (redir_entry != SIZE_MAX)
    {
        ioapic_irq_redirection_entry* ent = Arch_IRQRedirectionEntries + redir_entry;
        OBOS_ASSERT(ent->source == gsi);
        gsi = ent->globalSystemInterrupt;
    }
    ioapic_descriptor* ioapic = find_ioapic(gsi);
    if (!ioapic)
        return OBOS_STATUS_NOT_FOUND;
    volatile uint32_t ent[2];
    ent[0] = ((uint64_t)ArchH_IOAPICReadRegister(ioapic->address, OffsetOfReg(redirectionEntries[gsi-ioapic->gsi])));
    ent[1] = ((uint64_t)ArchH_IOAPICReadRegister(ioapic->address, OffsetOfReg(redirectionEntries[gsi-ioapic->gsi]) + 4));
    volatile ioapic_redirection_entry *entry = (ioapic_redirection_entry*)&ent;
    return entry->vector == 0 ? OBOS_STATUS_SUCCESS : OBOS_STATUS_IN_USE;
}
OBOS_PAGEABLE_FUNCTION void ArchH_IOAPICWriteRegister(ioapic* ioapic_, uint32_t offset, uint32_t value)
{
    volatile ioapic* const ioapic = ioapic_;
    OBOS_ASSERT(!(offset % 4));
    if (offset % 4)
        return;
    ioapic->ioregsel = offset/4;
    ioapic->iowin = value;
}
OBOS_PAGEABLE_FUNCTION uint32_t ArchH_IOAPICReadRegister(ioapic* ioapic_, uint32_t offset)
{
    volatile ioapic* const ioapic = ioapic_;
    ioapic->ioregsel = offset/4;
    return ioapic->iowin >> ((offset % 4)*8);
}
