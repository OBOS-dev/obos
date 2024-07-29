/*
 * oboskrnl/driver_interface/header.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

enum { OBOS_DRIVER_MAGIC = 0x00116d868ac84e59 };

typedef enum driver_header_flags
{
    DRIVER_HEADER_FLAGS_DETECT_VIA_ACPI = 0x1,
    DRIVER_HEADER_FLAGS_DETECT_VIA_PCI = 0x2,
    DRIVER_HEADER_FLAGS_NO_ENTRY = 0x4,
    DRIVER_HEADER_FLAGS_REQUEST_STACK_SIZE = 0x8,
} driver_header_flags;
typedef struct driver_header
{
    uint64_t magic;
    uint32_t flags;
    struct
    {
        uint32_t classCode;
        __uint128_t subclass;
        __uint128_t progIf;
    } pciId;
    struct
    {
        char hid[8];
        char cids[16][8];
    } acpiId;
    size_t stackSize; // If DRIVER_HEADER_FLAGS_REQUEST_STACK_SIZE is set.
} driver_header;