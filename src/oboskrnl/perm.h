/*
 * oboskrnl/perm.h
 *
 * Copyright (c) 2025 Omar Berrow
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

obos_status OBOS_CapabilityFetch(const char* id, capability* res, bool create);
obos_status OBOS_CapabilitySet(const char* id, const capability* perm, bool overwrite);
obos_status OBOS_CapabilityCheck(const char* id);
obos_status OBOS_CapabilityCheckAs(const char* id, uid user, gid group);