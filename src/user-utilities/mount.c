#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>

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

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("Usage: %s device target\n", argv[0]);
        return -1;
    }
    char* target = argv[2];
    char* device = argv[1];
    obos_status st = syscall2(Sys_Mount, target, device);
    if (obos_is_error(st))
    {
        errno = parse_file_status(st);
        perror("Sys_Mount");
        return -1;
    }

    return 0;
}