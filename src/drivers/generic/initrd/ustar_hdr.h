/*
 * drivers/generic/initrd/ustar_hdr.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

#include <driver_interface/header.h>

#include <vfs/vnode.h>

typedef struct ustar_hdr
{
    char filename[100];
    char filemode[8];
    char owner_uid[8];
    char group_uid[8];
    char filesize[12]; // In octal!
    char last_mod[12]; // In octal!
    char chksum[8];
    char type;
    char linked[100];
    char magic[6]; // should be ustar\0
    char version[2];
    char owner_uname[32];
    char group_uname[32];
    char unused[16];
    char prefix[155];
} OBOS_PACK ustar_hdr;

typedef struct initrd_inode {
    struct {
        struct initrd_inode *head, *tail;
        size_t nChildren;
    } children;
    struct initrd_inode *next, *prev, *parent;
    char* name;
    char* path;
    char* data;
    const char* linked_path;
    const ustar_hdr* hdr;
    size_t filesize;
    size_t path_len;
    size_t path_size;
    size_t name_len;
    size_t name_size;
    file_type type;
    uint32_t ino;
    bool persistent : 1;
    bool dead : 1;
    driver_file_perm perm;
    vnode* vnode;
} initrd_inode;

enum {
    FILEMODE_EXEC = BIT(0),
    FILEMODE_WRITE = BIT(1),
    FILEMODE_READ = BIT(2),
    FILEMODE_OTHER_EXEC = FILEMODE_EXEC << 0,
    FILEMODE_OTHER_WRITE = FILEMODE_WRITE << 0,
    FILEMODE_OTHER_READ = FILEMODE_READ << 0,
    FILEMODE_GROUP_EXEC = FILEMODE_EXEC << 3,
    FILEMODE_GROUP_WRITE = FILEMODE_WRITE << 3,
    FILEMODE_GROUP_READ = FILEMODE_READ << 3,
    FILEMODE_OWNER_EXEC = FILEMODE_EXEC << 6,
    FILEMODE_OWNER_WRITE = FILEMODE_WRITE << 6,
    FILEMODE_OWNER_READ = FILEMODE_READ << 6,
};
enum 
{
    AREGTYPE = '\0',
    REGTYPE = '0',
    LNKTYPE = '1',
    SYMTYPE = '2',
    CHRTYPE = '3',
    BLKTYPE = '4',
    DIRTYPE = '5',
    FIFOTYPE = '6',
    CONTYPE = '7',
};
#define USTAR_MAGIC "ustar\0"
