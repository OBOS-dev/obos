/*
 * drivers/generic/ahci/command.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <locks/event.h>

#include "structs.h"

struct ahci_phys_region
{
    uintptr_t phys;
    uint32_t sz : 22;
};
enum {
    COMMAND_DIRECTION_READ,
    COMMAND_DIRECTION_WRITE,
};
struct command_data
{
    struct ahci_phys_region* phys_regions;
    uint16_t physRegionCount;
    uint8_t direction;
    uint8_t cmd;
    bool awaitingSignal;
    // Set when the command is done.
    event completionEvent;
    obos_status commandStatus;
    struct {
        uint8_t cmdSlot;
    } internal;
};
obos_status SendCommand(Port* port, struct command_data* data, uint64_t lba, uint8_t device, uint16_t count);
obos_status ClearCommand(Port* port, struct command_data* data);
void StopCommandEngine(volatile HBA_PORT* port);
void StartCommandEngine(volatile HBA_PORT* port);

// Prevents any further transactions from happening.
// This causes SendComamnd to return OBOS_STATUS_RETRY, thus making it's callers also return that.
void HaltTranscations();
// Allows transactions to happen.
void ResumeTranscations();

// Wait for transcations to complete.
// If shutting down the ahci driver, or preparing to suspend,
// then call this after halting transactions.
void WaitForTranscations();
