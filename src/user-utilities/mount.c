#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>
#include <fcntl.h>

#include <obos/error.h>
#include <obos/syscall.h>

static int parse_file_status(obos_status status)
{
    switch (status)
    {
        case OBOS_STATUS_SUCCESS: return 0;
        case OBOS_STATUS_NOT_FOUND: return ENOENT;
        case OBOS_STATUS_INVALID_ARGUMENT: return EINVAL;
        case OBOS_STATUS_PAGE_FAULT: return EFAULT;
        case OBOS_STATUS_NOT_A_FILE: return EISDIR;
        case OBOS_STATUS_UNINITIALIZED: return EBADF;
        case OBOS_STATUS_EOF: return EIO;
        case OBOS_STATUS_ACCESS_DENIED: return EACCES;
        case OBOS_STATUS_NO_SYSCALL: return ENOSYS;
        case OBOS_STATUS_NOT_ENOUGH_MEMORY: return ENOSPC;
        case OBOS_STATUS_PIPE_CLOSED: return EPIPE;
        default: abort();
    }
}

const char* usage = "%s -h\n%s device target\n%s -t target\n%s -o overlay_path target\n%s -s devfs/initrd target\n";

enum {
    TMPFS_INITRD,
    TMPFS_DEVFS,
    TMPFS_NEW,
    TMPFS_OVERLAY,
};
#define Sys_OpenTmpFS 149

#define Sys_Mount2 150

int main(int argc, char** argv)
{
    int opt = 0;

    const char* target = NULL;
    const char* device = NULL;
    handle overlay = HANDLE_INVALID;
    
    bool tg_tmpfs = false;
    bool tg_tmpfs_system = false;
    int tmpfs_type = TMPFS_NEW;

    while ((opt = getopt(argc, argv, "+tosh")) != -1)
    {
        switch (opt)
        {
            case 't':
                tg_tmpfs = true;
                tmpfs_type = TMPFS_NEW;
                break;
            case 'o':
                tg_tmpfs = true;
                tmpfs_type = TMPFS_OVERLAY;
                break;
            case 's':
                tg_tmpfs = true;
                tg_tmpfs_system = true;
                tmpfs_type = TMPFS_DEVFS;
                break;
            case 'h':
            default:
                fprintf(stderr, usage, argv[0], argv[0], argv[0], argv[0], argv[0]);
                return opt != 'h';
        }
    }

    if (tmpfs_type == TMPFS_NEW && tg_tmpfs)
    {
        target = argv[optind];
        if (!target)
        {
            fprintf(stderr, usage, argv[0], argv[0], argv[0], argv[0], argv[0]);
            return 1;
        }
    }
    else
    {
        target = argv[optind+1];
        device = argv[optind];

        if (!device || !target)
        {
            fprintf(stderr, usage, argv[0], argv[0], argv[0], argv[0], argv[0]);
            return 1;
        }
    }

    if (tg_tmpfs_system)
    {
        if (strcasecmp(device, "devfs") == 0)
            tmpfs_type = TMPFS_DEVFS;
        else if (strcasecmp(device, "initrd") == 0)
            tmpfs_type = TMPFS_INITRD;
        else
        {
            fprintf(stderr, "Expected either initrd or devfs as target to %s -s\n", argv[0]);
            fprintf(stderr, usage, argv[0], argv[0], argv[0], argv[0], argv[0]);
            return 1;
        }
    }
    if (tmpfs_type == TMPFS_OVERLAY)
    {
        int fd = open(device, O_RDWR);
        if (fd < 0)
        {
            perror("open");
            return -1;
        }

        overlay = fd;
    }

    if (tg_tmpfs)
    {
        handle tmpfs = syscall0(Sys_FdAlloc);

        obos_status st = syscall4(Sys_OpenTmpFS, tmpfs, tmpfs_type, 0, overlay);
        if (obos_is_error(st))
        {
            errno = parse_file_status(st);
            perror("Sys_OpenTmpFS");
            return -1;
        }

        st = syscall2(Sys_Mount2, target, tmpfs);
        errno = parse_file_status(st);
        syscall1(Sys_HandleClose, tmpfs);
        if (obos_is_error(st))
        {
            perror("Sys_Mount2");
            return -1;
        }
    }
    else
    {
        obos_status st = syscall2(Sys_Mount, target, device);
        if (obos_is_error(st))
        {
            errno = parse_file_status(st);
            perror("Sys_Mount");
            return -1;
        }
    }

    return 0;
}