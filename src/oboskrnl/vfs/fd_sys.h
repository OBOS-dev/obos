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
obos_status      Sys_FdClose(handle desc);