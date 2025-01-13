/*
 * oboskrnl/vfs/init.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

// Initializes the VFS.
// After this is called, the root is set to the InitRD.
void Vfs_Initialize();
// Finalizes VFS initialization.
// To be called after fs drivers and disk drivers are done being loaded.
// This mainly mounts the root fs as was specified in the kernel cmd line.
// This also makes the special files:
// /dev/null
// /dev/zero
// /dev/full
void Vfs_FinalizeInitialization();
void Vfs_InitDummyDevices();
