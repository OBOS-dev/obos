/*
 * oboskrnl/perm.h
 *
 * Copyright (c) 2025-2026 Omar Berrow
 *
 * Syscall permission checking utilities
 */

#pragma once

#include <int.h>
#include <error.h>

#ifndef OBOS_PERM_PREFIX
#   define OBOS_PERM_PREFIX "/sys/perm/"
#endif

extern struct dirent* Vfs_PermRoot;

typedef struct capability {
    uid owner;
    gid group;
    bool allow_user : 1;
    bool allow_group : 1;
    bool allow_other : 1;
} capability;

void OBOS_CapabilityInitialize();

// NOTE: If a filesystem is unmounted, and remounted, any default values
// WILL be lost

// NOTE: OBOS_CapabilityCheckAs treats a capability like the following,
// if it cannot be found
// capability cap = {
//     .owner=ROOT_UID,
//     .group=ROOT_GID,
//     .allow_user=true,
//     .allow_group=true,
//     .allow_other=false
// };

OBOS_EXPORT obos_status OBOS_CapabilityFetch(const char* id, capability* res, bool create);
OBOS_EXPORT obos_status OBOS_CapabilitySet(const char* id, const capability* perm, bool overwrite);
OBOS_EXPORT obos_status OBOS_CapabilityCheck(const char* id, bool def_other_allow);
OBOS_EXPORT obos_status OBOS_CapabilityCheckAs(const char* id, uid user, gid group, bool def_other_allow);