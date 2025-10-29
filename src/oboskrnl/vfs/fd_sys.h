/*
 * oboskrnl/vfs/fd_sys.h
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>
#include <handle.h>
#include <signal.h>

#include <vfs/limits.h>
#include <vfs/irp.h>
#include <vfs/socket.h>

#include <driver_interface/header.h>

/// <summary>
/// Allocates a new handle with the type HANDLE_TYPE_FD (0)
/// </summary>
/// <returns>A new handle.</returns>
handle Sys_FdAlloc();

/// <summary>
/// Opens the file descriptor 'desc'
/// </summary>
/// <param name="desc">The file descriptor to open.</param>
/// <param name="path">The file to open</param>
/// <param name="oflags">Extra flags to specify how the file should be opened</param>
/// <returns>An obos_status.</returns>
/// <remarks>This function does not support FD_OFLAGS_CREAT! See Sys_FdOpenEx.</remarks>
obos_status Sys_FdOpen(handle desc, const char* path, uint32_t oflags);
/// <summary>
/// Opens the file descriptor 'desc'
/// </summary>
/// <param name="desc">The file descriptor to open.</param>
/// <param name="path">The file to open</param>
/// <param name="oflags">Extra flags to specify how the file should be opened</param>
/// <param name="mode">The file mode if the file is to be created</param>
/// <returns>An obos_status.</returns>
obos_status Sys_FdOpenEx(handle desc, const char* path, uint32_t oflags, uint32_t mode);
/// <summary>
/// Opens the file descriptor 'desc' on the directory entry 'ent'
/// </summary>
/// <param name="desc">The file descriptor to open.</param>
/// <param name="ent">The file to open</param>
/// <param name="oflags">Extra flags to specify how the file should be opened</param>
/// <returns>An obos_status.</returns>
obos_status Sys_FdOpenDirent(handle desc, handle ent, uint32_t oflags);
/// <summary>
/// Opens the file descriptor 'desc', but look for the file at realpath(ent)/path
/// </summary>
/// <param name="desc">The file descriptor to open.</param>
/// <param name="ent">The starting directory for the lookup</param>
/// <param name="path">The file to open</param>
/// <param name="oflags">Extra flags to specify how the file should be opened</param>
/// <returns>An obos_status.</returns>
/// <remarks>This function does not support FD_OFLAGS_CREAT! See Sys_FdOpenAtEx.</remarks>
obos_status Sys_FdOpenAt(handle desc, handle ent, const char* name, uint32_t oflags);
/// <summary>
/// Opens the file descriptor 'desc', but look for the file at realpath(ent)/path
/// </summary>
/// <param name="desc">The file descriptor to open.</param>
/// <param name="ent">The starting directory for the lookup</param>
/// <param name="path">The file to open</param>
/// <param name="oflags">Extra flags to specify how the file should be opened</param>
/// <param name="mode">The mode of the created file if FD_OFLAGS_CREAT is passed.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_FdOpenAtEx(handle desc, handle ent, const char* name, uint32_t oflags, uint32_t mode);
/// <summary>
/// Creates the file, then opens the file in 'desc'
/// </summary>
/// <param name="desc">The file descriptor to open.</param>
/// <param name="name">The path of the new file</param>
/// <param name="mode">The mode of the newly created file.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_FdCreat(handle desc, const char* name, uint32_t mode);
/// <summary>
/// Creates a directory
/// </summary>
/// <param name="name">The path of the new directory</param>
/// <param name="mode">The mode of the newly created directory.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_Mkdir(const char* name, uint32_t mode);
/// <summary>
/// Creates a directory
/// </summary>
/// <param name="name">The path of the new directory</param>
/// <param name="mode">The mode of the newly created directory.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_MkdirAt(handle dirent, const char* name, uint32_t mode);

/// <summary>
/// Writes bytes to an open file descriptor handle 'desc'.
/// </summary>
/// <param name="desc">The file descriptor to write.</param>
/// <param name="buf">The buffer to write.</param>
/// <param name="nBytes">The amount of bytes to write.</param>
/// <param name="nWritten">[out,optional] The amount of bytes written.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_FdWrite(handle desc, const void* buf, size_t nBytes, size_t* nWritten);
/// <summary>
/// Reads bytes from an open file descriptor handle 'desc'.
/// </summary>
/// <param name="desc">The file descriptor to read.</param>
/// <param name="buf">The buffer to read into.</param>
/// <param name="nBytes">The amount of bytes to read.</param>
/// <param name="nRead">[out,optional] The amount of bytes read.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_FdRead(handle desc, void* buf, size_t nBytes, size_t* nRead);

/// <summary>
/// Writes bytes to an open file descriptor handle 'desc' starting at offset.
/// </summary>
/// <param name="desc">The file descriptor to write.</param>
/// <param name="buf">The buffer to write.</param>
/// <param name="nBytes">The amount of bytes to write.</param>
/// <param name="nWritten">[out,optional] The amount of bytes written.</param>
/// <param name="offset">The amount of the write.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_FdPWrite(handle desc, const void* buf, size_t nBytes, size_t* nWritten, size_t offset);
/// <summary>
/// Reads bytes from an open file descriptor handle 'desc' starting at offset.
/// </summary>
/// <param name="desc">The file descriptor to read.</param>
/// <param name="buf">The buffer to read into.</param>
/// <param name="nBytes">The amount of bytes to read.</param>
/// <param name="nRead">[out,optional] The amount of bytes read.</param>
/// <param name="offset">The offset of the read.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_FdPRead(handle desc, void* buf, size_t nBytes, size_t* nRead, size_t offset);

// These functions are undocumented because of changes made to them in another branch.

// obos_status Sys_FdAWrite(handle desc, const void* buf, size_t nBytes, handle evnt);
// obos_status  Sys_FdARead(handle desc, void* buf, size_t nBytes, handle evnt);
// *file on entry is the file descriptor of the file to make an IRP for
// *file on exit is the handle to the irp
// if buffer is nullptr, the irp->dryOp is set to true
obos_status Sys_IRPCreate(handle *file, size_t offset, size_t size, enum irp_op operation, void* buffer);
obos_status Sys_IRPSubmit(handle irp);
// If close is true, the IRP is closed after waiting for it.
obos_status Sys_IRPWait(handle irp, obos_status* irp_status, size_t* nCompleted /* irp.nBlkRead/nBlkWritten */, bool close);
// Returns OBOS_STATUS_WOULD_BLOCK if the IRP has not completed, otherwise OBOS_STATUS_SUCCESS, or an error code.
obos_status Sys_IRPQueryState(handle irp);
obos_status Sys_IRPGetBuffer(handle irp, void** buff);
obos_status Sys_IRPGetStatus(handle irp, obos_status* irp_status, size_t* nCompleted /* irp.nBlkRead/nBlkWritten */);

/// <summary>
/// Changes the current offset of the file descriptor 'desc'.
/// </summary>
/// <param name="desc">The file descriptor to use.</param>
/// <param name="off">The new offset.</param>
/// <param name="whence">How the offset should be added to the descriptor.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_FdSeek(handle desc, off_t off, whence_t whence);
/// <summary>
/// Tells the current file offset of 'desc'
/// </summary>
/// <param name="desc">The file descriptor to query.</param>
/// <returns>The current file offset.</returns>
uoff_t Sys_FdTellOff(const handle desc);
/// <summary>
/// Tells the block size of the underlying vnode of 'desc'
/// </summary>
/// <param name="desc">The file descriptor to query.</param>
/// <returns>The vnode's block size.</returns>
size_t Sys_FdGetBlkSz(const handle desc);
/// <summary>
/// Queries the EOF status of the file
/// </summary>
/// <param name="desc">The file descriptor to query.</param>
/// <returns>OBOS_STATUS_SUCCESS if not EOF, OBOS_STATUS_EOF if EOF, otherwise another error.</returns>
obos_status Sys_FdEOF(const handle desc);

/// <summary>
/// Does an ioctl on the file descriptor 'desc'
/// </summary>
/// <param name="desc">The file descriptor to use.</param>
/// <param name="request">The request for the driver</param>
/// <param name="argp">The argument pointer</param>
/// <param name="sz_argp">The size of the data at argp</param>
/// <returns>An obos_status.</returns>
/// <remarks>If sz_argp is SIZE_MAX, then the function will query the size of argp from the driver</remarks>
obos_status Sys_FdIoctl(handle desc, uintptr_t request, void* argp, size_t sz_argp);

/// <summary>
/// Flushes a file descriptor
/// </summary>
/// <param name="desc">The file descriptor to flush.</param>
/// <returns>The vnode's block size.</returns>
obos_status Sys_FdFlush(handle desc);

/// <summary>
/// Flushes all file data to disk
/// </summary>
void Sys_Sync();

enum {
    FSFDT_PATH = 1,
    FSFDT_FD,
    FSFDT_FD_PATH,
};

#if defined(__x86_64__)
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
#elif defined(__m68k__)
struct OBOS_PACK stat {
	int64_t st_dev;
	unsigned char __st_dev_padding[2];
	unsigned long __st_ino;
	unsigned int st_mode;
	unsigned long st_nlink;
	uid st_uid;
	gid st_gid;
	int64_t st_rdev;
	unsigned char __st_rdev_padding[2];
	long long st_size; /* TODO: off64_t? */
	long st_blksize;
	int64_t st_blocks;
    uint64_t unused[3];
	uint64_t st_ino;
};
OBOS_STATIC_ASSERT(sizeof(struct stat) == 92, "sizeof(struct stat) should be 92");
#endif

#define AT_EMPTY_PATH 0x1000
#define AT_NO_AUTOMOUNT 0x800
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_SYMLINK_FOLLOW 0x400

/// <summary>
/// Stats a file.
/// </summary>
/// <param name="fsfdt">Target of the stat</param>
/// <param name="fd">Possible dirent or file descriptor to stat, depending on fsfdt</param>
/// <param name="path">Possible path of file to stat, depending on fsfdt</param>
/// <param name="flags">The stat flags</param>
/// <param name="target">[out] The file information</param>
/// <returns>An obos_status.</returns>
obos_status Sys_Stat(int fsfdt, handle fd, const char* path, int flags, struct stat* target);
/// <summary>
/// Stats a file system.
/// </summary>
/// <param name="dent">The root directory of the file system to be stated.</param>
/// <param name="info">[out] File system information.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_StatFSInfo(handle dent, drv_fs_info* info);

/// <summary>
/// Reads a symlink.
/// </summary>
/// <param name="parent">[optional, if set to AT_FDCWD] The parent directory of upath, or the file descriptor/dirent to query.</param>
/// <param name="upath">The path of the file</param>
/// <param name="ubuff">[out] The linked file path</param>
/// <param name="max_size">Maximum bytes to copy to ubuff</param>
/// <param name="length">[out] The amount of bytes copied in total.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_ReadLinkAt(handle parent, const char *upath, void* ubuff, size_t max_size, size_t* length);
/// <summary>
/// Unlinks ("deletes") an entry from the file system.
/// </summary>
/// <param name="parent">The parent directory of path.</param>
/// <param name="path">The path of the entry to delete</param>
/// <param name="flags">Deletion flags</param>
/// <returns>An obos_status.</returns>
obos_status Sys_UnlinkAt(handle parent, const char* path, int flags);

/// <summary>
/// Opens a directory.
/// </summary>
/// <param name="path">The path of the directory to be opened.</param>
/// <param name="status">[out] The status of the open.</param>
/// <returns>A new dirent (HANDLE_TYPE_DIRENT) handle.</returns>
handle Sys_OpenDir(const char* path, obos_status *status);
/// <summary>
/// Reads entries from a directory
/// </summary>
/// <param name="dent">The dirent to be queries.</param>
/// <param name="buffer">[out] Buffer to read into.</param>
/// <param name="szBuf">The size of the buffer</param>
/// <param name="nRead">[out] The amount of bytes read</param>
/// <returns>An obos_status.</returns>
obos_status Sys_ReadEntries(handle dent, void* buffer, size_t szBuf, size_t* nRead);

// Opens stdin, stdout, and stderr.
// Internal use only
void OBOS_OpenStandardFDs(struct handle_table* tbl);

/// <summary>
/// Mounts a file system.
/// </summary>
/// <param name="at">The root directory to mount on</param>
/// <param name="on">The file system to mount</param>
/// <returns>An obos_status.</returns>
obos_status Sys_Mount(const char* at, const char* on);
/// <summary>
/// Unmounts a file system.
/// </summary>
/// <param name="at">The directory to unmount</param>
/// <returns>An obos_status.</returns>
obos_status Sys_Unmount(const char* at);

/// <summary>
/// Changes the current working directory
/// </summary>
/// <param name="path">The new working directory.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_Chdir(const char *path);
/// <summary>
/// Changes the current working directory
/// </summary>
/// <param name="ent">The new working directory.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_ChdirEnt(handle ent);
/// <summary>
/// Queries the current working directory
/// </summary>
/// <param name="path">[out] The buffer to copy the path into.</param>
/// <param name="len">The length of said buffer.</param>
/// <returns>An obos_status.</returns>
obos_status Sys_GetCWD(char* path, size_t len);

/// <summary>
/// Creates a symbolic link 'link' to 'target'
/// </summary>
/// <param name="target">The target of the new link</param>
/// <param name="link">The path of the new link</param>
/// <returns>An obos_status.</returns>
obos_status Sys_SymLink(const char* target, const char* link);
/// <summary>
/// Creates a symbolic link dirfd/'link' to 'target'
/// </summary>
/// <param name="target">The target of the new link</param>
/// <param name="dirfd">The parent directory of the new link</param>
/// <param name="link">The path of the new link</param>
/// <returns>An obos_status.</returns>
obos_status Sys_SymLinkAt(const char* target, handle dirfd, const char* link);

obos_status Sys_LinkAt(handle olddirfd, const char *oldpath, handle newdirfd, const char *newpath, int flags);

struct pselect_extra_args
{
    /// <summary>
    /// The timeout (can be nullptr)
    /// </summary>
    const uintptr_t* timeout;
    /// <summary>
    /// The temporary signal mask (can be nullptr)
    /// </summary>
    const sigset_t* sigmask;
    /// <summary>
    /// [out] The number of signaled events
    /// </summary>
    int* num_events;
};

/// <summary>
/// A file descriptor passed to poll
/// </summary>
struct pollfd
{
    /// <summary>
    /// The file descriptor to poll
    /// </summary>
    handle fd;
    /// <summary>
    /// What events to poll
    /// </summary>
    short events;
    /// <summary>
    /// The events that were signaled
    /// </summary>
    short revents;
};

/// <summary>
/// Waits for file descriptors to be signaled
/// </summary>
/// <param name="nFds">The highest numbered file descriptor in any passed set.</param>
/// <param name="read_set">The set to listen for reads</param>
/// <param name="write_set">The set to listen for writes</param>
/// <param name="except_set">The set to listen for exceptional events (unimplemented)</param>
/// <param name="extra">Extra arguments to the function</param>
/// <returns>An obos_status.</returns>
/// <remarks>Use Sys_PPoll, or IRPs if possible, as pselect is an outdated interface</remarks>
obos_status Sys_PSelect(size_t nFds, uint8_t* read_set, uint8_t *write_set, uint8_t *except_set, const struct pselect_extra_args* extra);
/// <summary>
/// Waits for file descriptors to be signaled
/// </summary>
/// <param name="fds">The file descriptors to poll.</param>
/// <param name="nFds">The size of 'fds'.</param>
/// <param name="timeout">[optional, if set to nullptr] The timeout.</param>
/// <param name="sigmask">[optional, if set to nullptr] The temporary signal mask</param>
/// <param name="nEvents">[out, optional, if set to nullptr] The amount of events</param>
/// <returns>An obos_status.</returns>
obos_status Sys_PPoll(struct pollfd* fds, size_t nFds, const uintptr_t* timeout, const sigset_t* sigmask, int *nEvents);

/// <summary>
/// Creates an unnamed pipe
/// </summary>
/// <param name="ufds">[out, nElements=2] The buffer to store the new file descriptors.</param>
/// <param name="pipesize">The size of the new pipe</param>
/// <returns>An obos_status.</returns>
obos_status Sys_CreatePipe(handle* ufds, size_t pipesize);
/// <summary>
/// Creates a named pipe
/// </summary>
/// <param name="dirfd">Parent directory of path, unless set to AT_FDCWD.</param>
/// <param name="path">The path of the new FIFO.</param>
/// <param name="mode">The mode of the new FIFO.</param>
/// <param name="pipesize">The size of the new pipe</param>
/// <returns>An obos_status.</returns>
obos_status Sys_CreateNamedPipe(handle dirfd, const char* path, int mode, size_t pipesize);

/// <summary>
/// Modifies file descriptor settings.
/// </summary>
/// <param name="fd">The file descriptor to modify.</param>
/// <param name="request">The request</param>
/// <param name="args">Arguments for the request</param>
/// <param name="nArgs">Amount of arguments</param>
/// <param name="ret">Return value of the request</param>
/// <returns>An obos_status.</returns>
obos_status Sys_Fcntl(handle fd, int request, uintptr_t* args, size_t nArgs, int* ret);

/// <summary>
/// Modifies an inode's permissions
/// </summary>
/// <param name="dirfd">Parent directory of pathname, unless set to AT_FDCWD.</param>
/// <param name="pathname">The path of the file to modify.</param>
/// <param name="mode">The new file mode.</param>
/// <param name="flags">Extra flags</param>
/// <returns>An obos_status.</returns>
obos_status Sys_FChmodAt(handle dirfd, const char* pathname, int mode, int flags);
/// <summary>
/// Modifies an inode's owner
/// </summary>
/// <param name="dirfd">Parent directory of pathname, unless set to AT_FDCWD.</param>
/// <param name="pathname">The path of the file to modify.</param>
/// <param name="owner">The UID of the new owner.</param>
/// <param name="group">The GID of the new owner.</param>
/// <param name="flags">Extra flags</param>
/// <returns>An obos_status.</returns>
obos_status Sys_FChownAt(handle dirfd, const char *pathname, uid owner, gid group, int flags);

obos_status Sys_Socket(handle fd, int family, int type, int protocol);
struct sys_socket_io_params {
    sockaddr* sock_addr;
    // Untouched in sendto, modified in recvfrom
    size_t addr_length;
    // Only valid in recvfrom
    size_t nRead;
};
obos_status Sys_SendTo(handle fd, const void* buffer, size_t size, int flags, struct sys_socket_io_params *params);
obos_status Sys_RecvFrom(handle fd, void* buffer, size_t size, int flags, struct sys_socket_io_params *params);
obos_status Sys_Listen(handle fd, int backlog);
obos_status Sys_Accept(handle fd, handle new_fd, sockaddr* addr_ptr, size_t *addr_length, int flags);
obos_status Sys_Bind(handle fd, const sockaddr *addr, size_t addr_length);
obos_status Sys_Connect(handle fd, const sockaddr *addr, size_t addr_length);
obos_status Sys_SockName(handle fd, sockaddr* addr, size_t addr_length, size_t* actual_addr_length);
obos_status Sys_PeerName(handle fd, sockaddr* addr, size_t addr_length, size_t* actual_addr_length);
obos_status Sys_GetSockOpt(handle fd, int layer, int number, void *buffer, size_t *size);
obos_status Sys_SetSockOpt(handle fd, int layer, int number, const void *buffer, size_t size);
obos_status Sys_ShutdownSocket(handle fd, int how);
