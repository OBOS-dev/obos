/*
 * oboskrnl/driver_interface/header.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

enum { OBOS_DRIVER_MAGIC = 0x00116d868ac84e59 };
// Not required, but can speed up loading times if the driver header is put in here.
#define OBOS_DRIVER_HEADER_SECTION ".driverheader"

typedef enum driver_header_flags
{
    /// <summary>
    /// Should the driver be detected through ACPI?
    /// Cannot be specified if DETECT_VIA_PCI is specified.
    /// </summary>
    DRIVER_HEADER_FLAGS_DETECT_VIA_ACPI = 0x1,
    /// <summary>
    /// Should the driver be detected through PCI?
    /// Cannot be specified if DETECT_VIA_ACPI is specified.
    /// </summary>
    DRIVER_HEADER_FLAGS_DETECT_VIA_PCI = 0x2,
    /// <summary>
    /// If the driver does not have an entry point, specify this flag.
    /// </summary>
    DRIVER_HEADER_FLAGS_NO_ENTRY = 0x4,
    /// <summary>
    /// If set, the driver chooses its entry point's stack size.
    /// Ignored if DRIVER_HEADER_FLAGS_NO_ENTRY is set.
    /// </summary>
    DRIVER_HEADER_FLAGS_REQUEST_STACK_SIZE = 0x8,
} driver_header_flags;
typedef struct driver_header
{
    uint64_t magic;
    uint32_t flags;
    struct
    {
        uint32_t classCode;
        /// <summary>
        /// If a bit is set, the bit number will be the value.
        /// <para></para>
        /// This bitfield can have more than bit set (for multiple values).
        /// </summary>
        __uint128_t subclass;
        /// <summary>
        /// If a bit is set, the bit number will be the value.
        /// <para></para>
        /// This bitfield can have more than bit set (for multiple values).
        /// <para></para>
        /// If no bit is set any prog if is assumed.
        /// </summary>
        __uint128_t progIf;
    } pciId;
    struct
    {
        // These strings are not null-terminated.
        // The PnP IDs for the driver.
        // Each one of these is first compared with the HID.
        // Then, each one of these is compared with the CID.
        char pnpIds[32][8];
        // Ranges from 1-32 inclusive.
        size_t nPnpIds;
    } acpiId;
    size_t stackSize; // If DRIVER_HEADER_FLAGS_REQUEST_STACK_SIZE is set.
} driver_header;