/*
 * oboskrnl/driver_interface/loader.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <driver_interface/driverId.h>
#include <driver_interface/header.h>

#include <elf/elf.h>

#include <scheduler/thread.h>

// gets the driver header of an unloaded driver
obos_status Drv_LoadDriverHeader(void* file, size_t szFile, driver_header* header);
driver_id *Drv_LoadDriver(void* file, size_t szFile, obos_status* status);
obos_status Drv_StartDriver(driver_id* driver, thread** mainThread);
obos_status Drv_UnloadDriver(driver_id* driver);

// returns the base of the elf.
// if this resolves a symbol from a driver while relocating, then it must add the driver to the dependency list.
OBOS_WEAK void* DrvS_LoadRelocatableElf(driver_id* driver, void* file, size_t szFile, Elf_Sym** dynamicSymbolTable, size_t* nEntriesDynamicSymbolTable, const char** dynstrtab, void** top, obos_status* status);

// if the value set at *driver is nullptr, then the symbol is from the kernel
driver_symbol* DrvH_ResolveSymbol(const char* name, struct driver_id** driver);