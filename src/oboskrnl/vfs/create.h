/*
 * oboskrnl/vfs/create.h
 *
 * Copyright (c) 2025 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>

#include <driver_interface/header.h>

#include <vfs/vnode.h>
#include <vfs/dirent.h>

// obos_status VfsH_CreateNodeP(const char* parent, const char* name, uint32_t vtype, file_perm mode);
#define VfsH_CreateNodeP(parent, name, vtype, mode) ({\
    struct dirent*_parent = VfsH_DirentLookup(parent);\
    obos_status _status = OBOS_STATUS_SUCCESS;\
    if (!(_parent))\
        _status = (OBOS_STATUS_NOT_FOUND);\
    else\
        _status = Vfs_CreateNode(_parent, (name), (vtype), (mode));\
    (_status);\
})

obos_status Vfs_CreateNode(dirent* parent, const char* name, uint32_t vtype, file_perm mode);

OBOS_EXPORT obos_status Vfs_UnlinkNode(dirent* node);