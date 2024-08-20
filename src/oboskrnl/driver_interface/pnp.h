/*
 * oboskrnl/driver_interface/pnp.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <driver_interface/header.h>
#include <driver_interface/pci.h>

#include <vfs/dirent.h>

// every driver header in 'toLoad' is one found in 'what'
// nodes are allocated using the general purpose kernel allocator.
obos_status Drv_PnpDetectDrivers(driver_header_list what, driver_header_list *toLoad);
obos_status Drv_PnpLoadDriversAt(dirent* directory);