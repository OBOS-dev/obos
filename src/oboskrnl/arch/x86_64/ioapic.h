/*
 * oboskrnl/arch/x86_64/ioapic.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

typedef struct ioapic
{
	OBOS_ALIGNAS(0x10) volatile uint8_t ioregsel;
	OBOS_ALIGNAS(0x10) volatile uint32_t iowin;
} ioapic;
typedef struct OBOS_PACK ioapic_redirection_entry
{
    uint8_t vector; // the vector
    uint8_t delMod : 3; // delivery mode (1:1 as the lapic's delivery mode)
    bool destMode : 1; // the destination mode (logical: 1, physical: 0)
    const bool delivStatus : 1; // 1: send pending, 0: idle line
    bool intPol : 1; // polarity (0: active-high, 1: active-low)
    const bool remoteIRR : 1; // (level-triggered only): set to one when a lapic accepts the irq from the ioapic, and set to zero on EOI in the lapic.
    bool triggerMode : 1; // the irq trigger mode
    bool mask : 1; // whether the irq is masked
    const uint64_t padding : 39; // resv.
    union
    {
        struct
        {
            uint8_t setOfProcessors; // unused in obos
        } OBOS_PACK logical;
        struct
        {
            const uint8_t resv1 : 4; // resv.
            uint8_t lapicId : 4; // the lapic id.
        } OBOS_PACK physical;
    } OBOS_PACK destination; // the destination
} ioapic_redirection_entry;
OBOS_STATIC_ASSERT(sizeof(ioapic_redirection_entry) == 8, "sizeof(ioapic_redirection_entry) must be 8!");
typedef struct OBOS_PACK ioapic_registers
{
    struct {
        const uint32_t resv1 : 24;
        uint8_t ioapicID : 4;
        const uint8_t resv2 : 4;
    } OBOS_PACK ioapicId;
    struct
    {
        const uint8_t version;
        const uint8_t resv1;
        const uint8_t maximumRedirectionEntries;
        const uint8_t resv2;
    } OBOS_PACK ioapicVersion;
    struct
    {
        const uint32_t resv1 : 24;
        const uint8_t ioapicID : 4;
        const uint8_t resv2 : 4;
    } OBOS_PACK ioapicArbitrationID;
    uint32_t resv1[13];
    ioapic_redirection_entry redirectionEntries[];
} ioapic_registers;
typedef enum ioapic_trigger_mode
{
	TriggerModeEdgeSensitive = 0,
	TriggerModeLevelSensitive = 1,
} ioapic_trigger_mode;
typedef enum ioapic_polarity
{
    PolarityActiveHigh,
    PolarityActiveLow,
} ioapic_polarity;
typedef struct ioapic_irq_redirection_entry
{
    uint8_t source;
    uint32_t globalSystemInterrupt;
    ioapic_polarity polarity; // polarity (0: active-high, 1: active-low)
    ioapic_trigger_mode tm;
} ioapic_irq_redirection_entry;
typedef struct ioapic_descriptor
{
    uint8_t id;
    uintptr_t phys;
    ioapic* address;
    uint8_t maxRedirectionEntries;
    uint32_t gsi;
} ioapic_descriptor;
extern ioapic_irq_redirection_entry* Arch_IRQRedirectionEntries;
extern size_t Arch_SizeofIRQRedirectionEntries;
extern ioapic_descriptor* Arch_IOAPICs;
extern size_t Arch_IOAPICCount;
/// <summary>
/// Initialized all the I/O APICs of the system.
/// </summary>
/// <returns>The status of the function.</returns>
obos_status Arch_InitializeIOAPICs();
/// <summary>
/// Masks an IRQ on the I/O APIC.
/// </summary>
/// <param name="gsi">The gsi to mask.</param>
/// <param name="mask">false to unmask, true to mask.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status Arch_IOAPICMaskIRQ(uint32_t gsi, bool mask);
/// <summary>
/// Registers a GSI, redirecting it to a specific vector.
/// </summary>
/// <param name="gsi">The gsi to register.</param>
/// <param name="vector">The CPU vector to use. If this is zero, the IRQ is unregistered.</param>
/// <param name="polarity">The polarity of the IRQ.</param>
/// <param name="tm">The trigger mode of the IRQ.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status Arch_IOAPICMapIRQToVector(uint32_t gsi, uint8_t vector, ioapic_polarity polarity, ioapic_trigger_mode tm);
/// <summary>
/// Checks if a GSI is in use or not.
/// </summary>
/// <param name="gsi">The gsi to check.</param>
/// <returns>OBOS_STATUS_SUCCESS if it is unused, OBOS_STATUS_IN_USE if it is used, otherwise the status is an error.</returns>
OBOS_EXPORT obos_status Arch_IOAPICGSIUsed(uint32_t gsi);
/// <summary>
/// Writes an I/O APIC register.
/// </summary>
/// <param name="ioapic">The target ioapic.</param>
/// <param name="offset">The offset (in bytes) of the register. Must be divisible by 4</param>
/// <param name="value">The value to write.</param>
void ArchH_IOAPICWriteRegister(ioapic* ioapic, uint32_t offset, uint32_t value);
/// <summary>
/// Reads from an I/O APIC register.
/// </summary>
/// <param name="ioapic">The target ioapic.</param>
/// <param name="offset">The offset (in bytes) of the register. Doesn't have to be divisible by 4</param>
/// <returns>The value at the register offset.</returns>
uint32_t ArchH_IOAPICReadRegister(ioapic* ioapic, uint32_t offset);
