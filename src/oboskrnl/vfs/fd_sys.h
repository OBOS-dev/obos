/*
 * oboskrnl/vfs/fd_sys.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>
#include <handle.h>

#include <vfs/limits.h>

handle           Sys_FdAlloc();

obos_status       Sys_FdOpen(handle desc, const char* path, uint32_t oflags);
obos_status Sys_FdOpenDirent(handle desc, handle ent, uint32_t oflags);
obos_status     Sys_FdOpenAt(handle desc, handle ent, const char* name, uint32_t oflags);

obos_status      Sys_FdWrite(handle desc, const void* buf, size_t nBytes, size_t* nWritten);
obos_status       Sys_FdRead(handle desc, void* buf, size_t nBytes, size_t* nRead);

obos_status     Sys_FdAWrite(handle desc, const void* buf, size_t nBytes, handle evnt);
obos_status      Sys_FdARead(handle desc, void* buf, size_t nBytes, handle evnt);

obos_status       Sys_FdSeek(handle desc, off_t off, whence_t whence);
uoff_t         Sys_FdTellOff(const handle desc);
size_t        Sys_FdGetBlkSz(const handle desc);
obos_status        Sys_FdEOF(const handle desc);

obos_status      Sys_FdIoctl(handle desc, uint64_t request, void* argp, size_t sz_argp);

obos_status      Sys_FdFlush(handle desc);

enum {
    FSFDT_PATH = 1,
    FSFDT_FD,
    FSFDT_FD_PATH,
};

struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    unsigned long st_nlink;
    int st_mode;
    uid st_uid;
    gid st_gid;
    unsigned int __pad0;
    uint64_t st_rdev;
    off_t st_size;
    long st_blksize;
    int64_t st_blocks;
    // struct timespec st_atim;
    // struct timespec st_mtim;
    // struct timespec st_ctim;
    long resv[6];
    long __unused[3];
};

#define AT_EMPTY_PATH 0x1000
#define AT_NO_AUTOMOUNT 0x800
#define AT_SYMLINK_NOFOLLOW 0x100

obos_status Sys_Stat(int fsfdt, handle fd, const char* path, int flags, struct stat* target);

handle Sys_OpenDir(const char* path, obos_status *status);
obos_status Sys_ReadEntries(handle dent, void* buffer, size_t szBuf, size_t* nRead);

// Opens stdin, stdout, and stderr.
void OBOS_OpenStandardFDs(struct handle_table* tbl);
