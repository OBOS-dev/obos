/*
 * oboskrnl/vfs/fd_sys.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>
#include <handle.h>
#include <signal.h>

#include <vfs/limits.h>
#include <vfs/irp.h>

#include <driver_interface/header.h>

handle Sys_FdAlloc();

obos_status       Sys_FdOpen(handle desc, const char* path, uint32_t oflags);
obos_status     Sys_FdOpenEx(handle desc, const char* path, uint32_t oflags, uint32_t mode);
obos_status Sys_FdOpenDirent(handle desc, handle ent, uint32_t oflags);
obos_status     Sys_FdOpenAt(handle desc, handle ent, const char* name, uint32_t oflags);
obos_status   Sys_FdOpenAtEx(handle desc, handle ent, const char* name, uint32_t oflags, uint32_t mode);
obos_status      Sys_FdCreat(handle desc, const char* name, uint32_t mode);
obos_status        Sys_Mkdir(const char* name, uint32_t mode);
obos_status      Sys_MkdirAt(handle dirent, const char* name, uint32_t mode);

obos_status Sys_FdWrite(handle desc, const void* buf, size_t nBytes, size_t* nWritten);
obos_status  Sys_FdRead(handle desc, void* buf, size_t nBytes, size_t* nRead);

// obos_status Sys_FdAWrite(handle desc, const void* buf, size_t nBytes, handle evnt);
// obos_status  Sys_FdARead(handle desc, void* buf, size_t nBytes, handle evnt);
handle Sys_IRPCreate(handle file, size_t offset, size_t size, bool dry, enum irp_op operation, void* buffer, obos_status* status);
obos_status Sys_IRPSubmit(handle irp);
// If close is true, the IRP is closed after waiting for it.
obos_status Sys_IRPWait(handle irp, obos_status* irp_status, size_t* nCompleted /* irp.nBlkRead/nBlkWritten */, bool close);
// Returns OBOS_STATUS_WOULD_BLOCK if the IRP has not completed, otherwise OBOS_STATUS_SUCCESS, or an error code.
obos_status Sys_IRPQueryState(handle irp);
obos_status Sys_IRPGetBuffer(handle irp, void** buff);
obos_status Sys_IRPGetStatus(handle irp, obos_status* irp_status, size_t* nCompleted /* irp.nBlkRead/nBlkWritten */);

obos_status Sys_FdSeek(handle desc, off_t off, whence_t whence);
uoff_t   Sys_FdTellOff(const handle desc);
size_t  Sys_FdGetBlkSz(const handle desc);
obos_status  Sys_FdEOF(const handle desc);

obos_status Sys_FdIoctl(handle desc, uint64_t request, void* argp, size_t sz_argp);

obos_status Sys_FdFlush(handle desc);

void Sys_Sync();

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
obos_status Sys_StatFSInfo(handle dent, drv_fs_info* info);

obos_status Sys_ReadLinkAt(handle parent, const char *upath, void* ubuff, size_t max_size, size_t* length);

handle Sys_OpenDir(const char* path, obos_status *status);
obos_status Sys_ReadEntries(handle dent, void* buffer, size_t szBuf, size_t* nRead);

// Opens stdin, stdout, and stderr.
void OBOS_OpenStandardFDs(struct handle_table* tbl);

obos_status Sys_Mount(const char* at, const char* on);
obos_status Sys_Unmount(const char* at);

obos_status Sys_Chdir(const char *path);
obos_status Sys_ChdirEnt(handle ent);
obos_status Sys_GetCWD(char* path, size_t len);

struct pselect_extra_args
{
    const uintptr_t* timeout;
    const sigset_t* sigmask;
    int* num_events;
};

obos_status Sys_PSelect(size_t nFds, uint8_t* read_set, uint8_t *write_set, uint8_t *except_set, const struct pselect_extra_args* extra);

obos_status Sys_CreatePipe(handle* ufds, size_t pipesize);