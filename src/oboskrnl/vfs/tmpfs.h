/*
 * oboskrnl/vfs/tmpfs.h
 *
 * Copyright (c) 2026 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/vnode.h>
#include <vfs/dirent.h>

#include <driver_interface/header.h>

typedef struct tmpfs {
    driver_header* backend;
    vnode* backend_fs;

    dirent* root;
    vnode* obj;

    uint32_t next_unused_inode;
} tmpfs;

obos_status Vfs_TmpFSCreate(vnode** tmpfs, driver_header* backend, void* backend_fs);
obos_status Vfs_TmpFSInitializeSpecial(tmpfs** out, driver_header* backend, void* backend_fs);
// Initializes the necessary vnode fields in 'vn' for it to be used in the tmpfs 'fs'.
obos_status Vfs_TmpFSMakeVnode(tmpfs* fs, vnode* vn, dirent* ent);

extern driver_id OBOS_TmpFSDriver;