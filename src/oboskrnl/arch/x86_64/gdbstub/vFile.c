/*
 * oboskrnl/arch/x86_64/gdbstub/vFile.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <handle.h>
#include <memmanip.h>
#include <cmdline.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>
#include <vfs/dirent.h>

#include <arch/x86_64/gdbstub/connection.h>
#include <arch/x86_64/gdbstub/packet_dispatcher.h>
#include <arch/x86_64/gdbstub/alloc.h>

#include <uacpi_libc.h>

#define GDB_EPERM           1
#define GDB_ENOENT          2
#define GDB_EINTR           4
#define GDB_EBADF           9
#define GDB_EACCES         13
#define GDB_EFAULT         14
#define GDB_EBUSY          16
#define GDB_EEXIST         17
#define GDB_ENODEV         19
#define GDB_ENOTDIR        20
#define GDB_EISDIR         21
#define GDB_EINVAL         22
#define GDB_ENFILE         23
#define GDB_EMFILE         24
#define GDB_EFBIG          27
#define GDB_ENOSPC         28
#define GDB_ESPIPE         29
#define GDB_EROFS          30
#define GDB_ENOSYS         38 /* this actually isn't listed in the remote protocol lol */
#define GDB_ENAMETOOLONG   91
#define GDB_EUNKNOWN       9999

static int obos_status_to_gdb_errno(obos_status status)
{
    switch (status) {
        case OBOS_STATUS_SUCCESS: return 0; // success :)
        case OBOS_STATUS_ACCESS_DENIED: return GDB_EACCES;
        case OBOS_STATUS_NOT_FOUND: return GDB_ENOENT;
        case OBOS_STATUS_RETRY: return GDB_EINTR;
        case OBOS_STATUS_TIMED_OUT: return GDB_EINTR;
        case OBOS_STATUS_UNINITIALIZED: return GDB_EBADF;
        case OBOS_STATUS_PAGE_FAULT: return GDB_EFAULT;
        case OBOS_STATUS_WOULD_BLOCK: return GDB_EBUSY;
        case OBOS_STATUS_ALREADY_MOUNTED: return GDB_EBUSY;
        case OBOS_STATUS_ALREADY_INITIALIZED: return GDB_EEXIST;
        case OBOS_STATUS_NOT_A_FILE: return GDB_EISDIR;
        case OBOS_STATUS_INVALID_ARGUMENT: return GDB_EINVAL;
        case OBOS_STATUS_NO_SPACE: return GDB_ENOSPC;
        case OBOS_STATUS_READ_ONLY: return GDB_EROFS;
        case OBOS_STATUS_UNIMPLEMENTED: return GDB_ENOSYS;
        default: OBOS_Log("Kdbg: vFile: Function returned status %d which cannot be translated to an errno.\n", status); return GDB_EUNKNOWN;
    }
}

handle_table Kdbg_GDBHandleTable;

#define O_RDONLY        0x0
#define O_WRONLY        0x1
#define O_RDWR          0x2
#define O_APPEND        0x8
#define O_CREAT       0x200
#define O_TRUNC       0x400
#define O_EXCL        0x800

#define S_IFLNK       0120000
#define S_IFREG       0100000
#define S_IFBLK       0060000
#define S_IFDIR       0040000
#define S_IFCHR       0020000
#define S_IFIFO       0010000
#define S_IRUSR          0400
#define S_IWUSR          0200
#define S_IXUSR          0100
#define S_IRGRP           040
#define S_IWGRP           020
#define S_IXGRP           010
#define S_IROTH            04
#define S_IWOTH            02
#define S_IXOTH            01


static char* hex2str(const char* hex, size_t len)
{
    char* ret = Kdbg_Malloc(len/2+1);
    ret[len/2] = 0;
    for (size_t i = 0; i < len; i += 2)
        ret[i/2] = (char)KdbgH_hex2bin(hex+i, 2);
    return ret;
}

#define do_bounds_check(ptr, pstart, len) \
({\
    uintptr_t _start = (uintptr_t)pstart;\
    uintptr_t _p = (uintptr_t)ptr;\
    (_p >= _start) && (_p < (_start+len));\
})

static const char* next_escaped_char(const char* iter, char* end)
{
    while (iter++ < end)
    {
        switch (*iter) {
            case '#':
            case '$':
            case '}':
                return iter;
            default: continue;
        }
    }
    return end;
}
static char* format_binary_response(const char* buf, char* resp, size_t initial_resp_offset, size_t* const resp_len, size_t sz_buf)
{
    const char* iter = buf;
    char* resp_iter = resp+initial_resp_offset;
    while (iter < (char*)buf+sz_buf)
    {
        size_t nCopied = 0;
        if (*iter == '#' || *iter == '$' || *iter == '}')
        {
            (*resp_len)++;
            char* prev_resp = resp;
            resp = Kdbg_Realloc(resp, (*resp_len));
            resp_iter = resp + (resp_iter - prev_resp);
            *resp_iter++ = '}';
            *resp_iter++ = *iter ^ 0x20;
            iter++;
            continue;
        }
        else
        {
            const char* next_ch = next_escaped_char(iter, (char*)buf+sz_buf);
            nCopied = next_ch - iter;
        }
        memcpy(resp_iter, buf, nCopied);
        iter += nCopied;
        resp_iter += nCopied;
    }
    return resp;
}

struct gdb_stat {
    unsigned int  st_dev;      /* device */
    unsigned int  st_ino;      /* inode */
    unsigned int  st_mode;     /* protection */
    unsigned int  st_nlink;    /* number of hard links */
    unsigned int  st_uid;      /* user ID of owner */
    unsigned int  st_gid;      /* group ID of owner */
    unsigned int  st_rdev;     /* device type (if inode device) */
    unsigned long st_size;     /* total size, in bytes */
    unsigned long st_blksize;  /* blocksize for filesystem I/O */
    unsigned long st_blocks;   /* number of blocks allocated */
    long          st_atime;    /* time of last access */
    long          st_mtime;    /* time of last modification */
    long          st_ctime;    /* time of last change */
};

static obos_status common_stat(vnode* vn, struct gdb_stat* out)
{
    OBOS_ENSURE(vn && out);
    switch (vn->vtype) {
        case VNODE_TYPE_DIR:
            out->st_mode |= S_IFDIR;
            break;
        case VNODE_TYPE_FIFO:
            out->st_mode |= S_IFIFO;
            break;
        case VNODE_TYPE_CHR:
            out->st_mode |= S_IFCHR;
            break;
        case VNODE_TYPE_BLK:
            out->st_mode |= S_IFBLK;
            break;
        case VNODE_TYPE_REG:
            out->st_mode |= S_IFREG;
            break;
        case VNODE_TYPE_LNK:
            out->st_mode |= S_IFLNK;
            break;
        default:
            return OBOS_STATUS_UNIMPLEMENTED;
    }
    out->st_mode |= vn->perm.mode;
    out->st_dev = 0;
    out->st_ino = vn->inode;
    out->st_nlink = vn->refs;
    out->st_uid = vn->owner_uid;
    out->st_gid = vn->group_uid;
    out->st_rdev = 0;
    out->st_size = vn->filesize;
    out->st_blksize = vn->blkSize;
    out->st_blocks = vn->filesize/vn->blkSize;
    out->st_atime = 0;
    out->st_mtime = 0;
    out->st_ctime = 0;
    return OBOS_STATUS_SUCCESS;
}

obos_status Kdbg_GDB_vFile(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata)
{
    OBOS_UNUSED(userdata);
    OBOS_UNUSED(ctx);

    if (!Kdbg_GDBHandleTable.arr)
        OBOS_InitializeHandleTable(&Kdbg_GDBHandleTable);

    const char* op = arguments;
    size_t op_len = strnchr(op, ':', argumentsLen)-1;
    if (op_len == argumentsLen)
    {
        Kdbg_ConnectionSendPacket(con, "");
        return OBOS_STATUS_SUCCESS;
    }
    const char* op_args = op+op_len+1;
    size_t op_args_len = argumentsLen - (op_len+1);
    // NOTE(oberrow): Intentionally don't support pwrite and unlink

    if (uacpi_strncmp(op, "open", op_len) == 0)
    {
        char* filename = nullptr;
        size_t filename_len = 0;
        int flags = 0;
        // NOTE: There is a mode, but we're going to ignore that.

        if (!do_bounds_check(op_args, op_args, op_args_len))
        {
            Kdbg_ConnectionSendPacket(con, "F-1,16"); // EINVAL
            return OBOS_STATUS_SUCCESS;
        }

        filename_len = strnchr(op_args, ',', op_args_len) / 2;
        filename = hex2str(op_args, filename_len*2);

        const char* flags_ptr = op_args + (filename_len*2)+1;
        OBOS_MAYBE_UNUSED size_t flags_ptr_len = strnchr(op_args+((filename_len*2)+1), ',', op_args_len-((filename_len*2)+1));

        if (!do_bounds_check(flags_ptr, op_args, op_args_len))
        {
            Kdbg_Free(filename);
            Kdbg_ConnectionSendPacket(con, "F-1,16"); // EINVAL
            return OBOS_STATUS_SUCCESS;
        }

        flags = OBOSH_StrToULL(flags_ptr, nullptr, 16);

        uint32_t decoded_flags = 0;
        if ((flags & 3) == O_RDONLY)
            decoded_flags |= FD_OFLAGS_READ;
        else if ((flags & 3) == O_WRONLY)
            decoded_flags |= FD_OFLAGS_WRITE;
        else if ((flags & 3) == O_RDWR)
            decoded_flags |= FD_OFLAGS_READ|FD_OFLAGS_WRITE;
        
        handle_desc *desc = {};
        OBOS_LockHandleTable(&Kdbg_GDBHandleTable);
        handle fd = OBOS_HandleAllocate(&Kdbg_GDBHandleTable, HANDLE_TYPE_FD, &desc);
        desc->un.fd = Kdbg_Calloc(1, sizeof(fd));
        int errno = obos_status_to_gdb_errno(Vfs_FdOpen(desc->un.fd, filename, decoded_flags));
        if (flags & O_APPEND && !errno)
            Vfs_FdSeek(desc->un.fd, 0, SEEK_END);
        OBOS_UnlockHandleTable(&Kdbg_GDBHandleTable);
        
        char* resp = nullptr;
        
        if (errno != 0)
            resp = KdbgH_FormatResponse("F-1,%x", errno);
        else
            resp = KdbgH_FormatResponse("F%x", fd);

        Kdbg_ConnectionSendPacket(con, resp);
        
        Kdbg_Free(filename);
        Kdbg_Free(resp);
    }
    else if (uacpi_strncmp(op, "close", op_len) == 0)
    {
        if (!op_args_len)
            return Kdbg_ConnectionSendPacket(con, "");
        handle fd = OBOSH_StrToULL(op_args, nullptr, 0);

        int errno = obos_status_to_gdb_errno(Sys_HandleClose(fd));
        
        char* resp = nullptr;
        if (errno != 0)
            resp = KdbgH_FormatResponse("F-1,%x", errno);
        else
            resp = KdbgH_FormatResponse("F0");

        Kdbg_ConnectionSendPacket(con, resp);
        Kdbg_Free(resp);
    }
    else if (uacpi_strncmp(op, "pread", op_len) == 0)
    {
        handle desc = 0;
        size_t count = 0, offset = 0;

        const char* args_iter = op_args;
        do {
            const char* ptr = args_iter;
            if (!do_bounds_check(ptr, op_args, op_args_len))
                return Kdbg_ConnectionSendPacket(con, "F-1,16"); // EINVAL
            desc = OBOSH_StrToULL(ptr, &args_iter, 16);
            args_iter++;
        } while(0);
        do {
            const char* ptr = args_iter;
            if (!do_bounds_check(ptr, op_args, op_args_len))
                return Kdbg_ConnectionSendPacket(con, "F-1,16"); // EINVAL
            count = OBOSH_StrToULL(ptr, &args_iter, 16);
            args_iter++;
        } while(0);
        do {
            const char* ptr = args_iter;
            if (!do_bounds_check(ptr, op_args, op_args_len))
                return Kdbg_ConnectionSendPacket(con, "F-1,16"); // EINVAL
            offset = OBOSH_StrToULL(ptr, &args_iter, 16);
            args_iter++;
        } while(0);

        OBOS_LockHandleTable(&Kdbg_GDBHandleTable);
        handle_desc *fd = nullptr;
        obos_status status = OBOS_STATUS_SUCCESS;
        fd = OBOS_HandleLookup(&Kdbg_GDBHandleTable, desc, HANDLE_TYPE_FD, false, &status);
        int errno = 0;
        if (obos_is_error(status))
        {
            errno = obos_status_to_gdb_errno(status);
            char* resp = KdbgH_FormatResponse("F%d,%x", errno == 0 ? 0 : -1, errno);
            Kdbg_ConnectionSendPacket(con, resp);
            Kdbg_Free(resp);
            goto down;
        }
        OBOS_UnlockHandleTable(&Kdbg_GDBHandleTable);

        void* buf = Kdbg_Malloc(count);
        size_t nRead = 0;
        errno = obos_status_to_gdb_errno(Vfs_FdPRead(fd->un.fd, buf, offset, count, &nRead));

        char* resp = nullptr;
        if (errno == 0)
        {
            resp = KdbgH_FormatResponse("F%x;", nRead);
            size_t resp_len = strlen(resp);
            resp = Kdbg_Realloc(resp, resp_len+nRead);
            resp_len += nRead;
            resp = format_binary_response(buf, resp, resp_len-nRead, &resp_len, nRead);
        }
        else
            resp = KdbgH_FormatResponse("F-1,%d;", errno);
        Kdbg_ConnectionSendPacket(con, resp);
        Kdbg_Free(resp);
    }
    else if (uacpi_strncmp(op, "fstat", op_len) == 0)
    {
        handle desc = 0;
        const char* args_iter = op_args;
        size_t rel_args_size = op_args_len;
        do {
            const char* ptr = args_iter;
            size_t len_ptr = rel_args_size;
            if (!do_bounds_check(ptr, op_args, op_args_len))
                return Kdbg_ConnectionSendPacket(con, "F-1,16"); // EINVAL
            desc = OBOSH_StrToULL(ptr, nullptr, 0);
            rel_args_size -= len_ptr - 1;
            args_iter += len_ptr + 1;
        } while(0);

        OBOS_LockHandleTable(&Kdbg_GDBHandleTable);
        handle_desc *fd = nullptr;
        obos_status status = OBOS_STATUS_SUCCESS;
        fd = OBOS_HandleLookup(&Kdbg_GDBHandleTable, desc, HANDLE_TYPE_FD, false, &status);
        int errno = 0;
        if (obos_is_error(status))
        {
            errno = obos_status_to_gdb_errno(status);
            char* resp = KdbgH_FormatResponse("F-1,%x", errno);
            Kdbg_ConnectionSendPacket(con, resp);
            Kdbg_Free(resp);
            goto down;
        }
        OBOS_UnlockHandleTable(&Kdbg_GDBHandleTable);

        struct gdb_stat st = {};
        errno = obos_status_to_gdb_errno(common_stat(fd->un.fd->vn, &st));
     
        char* resp = nullptr;
        if (errno == 0)
        {
            resp = KdbgH_FormatResponse("F%x;", sizeof(st));
            size_t resp_len = strlen(resp);
            resp = Kdbg_Realloc(resp, resp_len+sizeof(st));
            resp_len += sizeof(st);
            resp = format_binary_response((void*)&st, resp, resp_len-sizeof(st), &resp_len, sizeof(st));
        }
        else
            resp = KdbgH_FormatResponse("F-1,%x;", errno);
        Kdbg_ConnectionSendPacket(con, resp);
        Kdbg_Free(resp);
    }
    else if (uacpi_strncmp(op, "stat", op_len) == 0)
    {
        vnode* vn = nullptr;
        char* filename = nullptr;
        size_t filename_len = 0;

        if (!do_bounds_check(op_args, op_args, op_args_len))
        {
            Kdbg_ConnectionSendPacket(con, "F-1,16"); // EINVAL
            return OBOS_STATUS_SUCCESS;
        }

        filename_len = op_args_len / 2;
        filename = hex2str(op_args, filename_len*2);

        dirent* ent = VfsH_DirentLookup(filename);
        if (!ent)
        {
            Kdbg_Free(filename);
            Kdbg_ConnectionSendPacket(con, "F-1,2"); // ENOENT
            return OBOS_STATUS_SUCCESS;
        }
        ent = VfsH_FollowLink(ent);
        vn = ent->vnode;

        struct gdb_stat st = {};
        int errno = obos_status_to_gdb_errno(common_stat(vn, &st));
     
        char* resp = nullptr;
        if (errno == 0)
        {
            resp = KdbgH_FormatResponse("F%d;", sizeof(st));
            size_t resp_len = strlen(resp);
            resp = Kdbg_Realloc(resp, resp_len+sizeof(st));
            resp_len += sizeof(st);
            resp = format_binary_response((void*)&st, resp, resp_len-sizeof(st), &resp_len, sizeof(st));
        }
        else
            resp = KdbgH_FormatResponse("F-1,%d;", errno);
        Kdbg_ConnectionSendPacket(con, resp);
        Kdbg_Free(resp);
        Kdbg_Free(filename);
    }
    else if (uacpi_strncmp(op, "lstat", op_len) == 0)
    {
        vnode* vn = nullptr;
        char* filename = nullptr;
        size_t filename_len = 0;

        if (!do_bounds_check(op_args, op_args, op_args_len))
        {
            Kdbg_ConnectionSendPacket(con, "F-1,16"); // EINVAL
            return OBOS_STATUS_SUCCESS;
        }

        filename_len = op_args_len / 2;
        filename = hex2str(op_args, filename_len*2);

        dirent* ent = VfsH_DirentLookup(filename);
        if (!ent)
        {
            Kdbg_Free(filename);
            Kdbg_ConnectionSendPacket(con, "F-1,2"); // ENOENT
            return OBOS_STATUS_SUCCESS;
        }
        vn = ent->vnode;

        struct gdb_stat st = {};
        int errno = obos_status_to_gdb_errno(common_stat(vn, &st));
     
        char* resp = nullptr;
        if (errno == 0)
        {
            resp = KdbgH_FormatResponse("F%x;", sizeof(st));
            size_t resp_len = strlen(resp);
            resp = Kdbg_Realloc(resp, resp_len+sizeof(st));
            resp_len += sizeof(st);
            resp = format_binary_response((void*)&st, resp, resp_len-sizeof(st), &resp_len, sizeof(st));
        }
        else
            resp = KdbgH_FormatResponse("F-1,%x;", errno);
        Kdbg_ConnectionSendPacket(con, resp);
        Kdbg_Free(resp);
        Kdbg_Free(filename);
    }
    else if (uacpi_strncmp(op, "readlink", op_len) == 0)
    {
        char* filename = nullptr;
        size_t filename_len = 0;

        if (!do_bounds_check(op_args, op_args, op_args_len))
        {
            Kdbg_ConnectionSendPacket(con, "F-1,16"); // EINVAL
            return OBOS_STATUS_SUCCESS;
        }

        filename_len = op_args_len / 2;
        filename = hex2str(op_args, filename_len*2);

        dirent* ent = VfsH_DirentLookup(filename);
        if (!ent)
        {
            Kdbg_Free(filename);
            Kdbg_ConnectionSendPacket(con, "F-1,2"); // ENOENT
            return OBOS_STATUS_SUCCESS;
        }
        
        int errno = 0;
        const char* lnk_dat = ent->vnode->un.linked;
        size_t len_lnk_dat = strlen(lnk_dat);

        char* resp = nullptr;
        if (errno == 0)
        {
            resp = KdbgH_FormatResponse("F%x;", len_lnk_dat);
            size_t resp_len = strlen(resp);
            resp = Kdbg_Realloc(resp, resp_len+len_lnk_dat);
            resp_len += len_lnk_dat;
            resp = format_binary_response(lnk_dat, resp, resp_len-len_lnk_dat, &resp_len, len_lnk_dat);
        }
        else
            resp = KdbgH_FormatResponse("F-1,%x;", errno);
        Kdbg_ConnectionSendPacket(con, resp);
        Kdbg_Free(resp);
        Kdbg_Free(filename);
    }
    else if (uacpi_strncmp(op, "setfs", op_len) == 0)
        return Kdbg_ConnectionSendPacket(con, "F0");
    else
        Kdbg_ConnectionSendPacket(con, "");
    down:
    return OBOS_STATUS_SUCCESS;
}