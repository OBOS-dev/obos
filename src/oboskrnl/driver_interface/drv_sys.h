/*
 * oboskrnl/driver_interface/drv_sys.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>
#include <handle.h>

handle Sys_LoadDriver(const void* file, size_t szFile, obos_status* status);
obos_status Sys_StartDriver(handle driver, handle* mainThread);
obos_status Sys_UnloadDriver(handle driver);

obos_status Sys_PnpLoadDriversAt(handle dent, bool wait);

// Finds a loaded driver by its name, and returns a handle to it.
// If the calling process has insufficient permissions, HANDLE_INVALID is returned.
handle Sys_FindDriverByName(const char* name /* assumed to be 64-bytes at max */);

// Returns the next driver in the list
// The handle 'curr' is not automatically closed.
// If 'curr' is HANDLE_INVALID, the first item in the list is returned.
// If the calling process has insufficient permissions, HANDLE_INVALID is returned.
handle Sys_EnumerateLoadedDrivers(handle curr);

// Queries the name of the driver in 'drv'.
// sznamebuf shall never be NULL.
// sznamebuf is the size of the buffer passed, and *sznamebuf is set to the actual length of the string.
// When copying the buffer, MIN(*sznamebuf, strlen(driverName)) is chosen.
obos_status Sys_QueryDriverName(handle drv, char* namebuf, size_t *sznamebuf /* need not be over 64 */);

obos_status Sys_GetHDADevices(handle* arr, size_t* count, uint32_t oflags);