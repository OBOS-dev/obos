/*
 * init/main.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <unistd.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/wait.h>
#include <sys/select.h>

#include <obos/syscall.h>
#include <obos/error.h>

#include "nm.h"

int print_motd();

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

const char* sigchld_action = "shutdown";

bool is_power_button_handler = false;

// NOTE: This might need to be changed if init starts "adopting" processes when their parents die (as of this commit, the kernel adopts them).
void sigchld_handler(int num)
{
    (void)(num);
    if (!is_power_button_handler)
        printf("init: Child process died. Performing sigchld action \"%s\"\n", sigchld_action);
    sync();
    if (strcasecmp(sigchld_action, "shutdown") == 0)
        syscall0(Sys_Shutdown);
    else if (strcasecmp(sigchld_action, "reboot") == 0)
        syscall0(Sys_Reboot);
    else if (strcasecmp(sigchld_action, "suspend") == 0)
        syscall0(Sys_Suspend);
    else if (strcasecmp(sigchld_action, "ignore") == 0)
        return;
    else
        abort(); // bug

    exit(0);
}

int main(int argc, char** argv)
{
    if (getpid() != 1)
        return -1;
    const char* swap_file = NULL;
    char* handoff_process = NULL;
    int opt = 0;
    while ((opt = getopt(argc, argv, "+s:c:h")) != -1)
    {
        switch (opt)
        {
            case 'c':
            {
                sigchld_action = optarg;
                // what the hell is this?
                if (!sigchld_action ||
                    (strcasecmp(sigchld_action, "help") == 0 && 
                    (
                        strcasecmp(sigchld_action, "shutdown") != 0 && 
                        strcasecmp(sigchld_action, "reboot") != 0 && 
                        strcasecmp(sigchld_action, "suspend") != 0 && 
                        strcasecmp(sigchld_action, "ignore") != 0)
                    )
                )
                {
                    fprintf(stderr, "-c valid values: 'shutdown', 'reboot', 'suspend', 'ignore', and 'help' (for this menu).\n");
                    return !sigchld_action ? 1 : strcasecmp(sigchld_action, "help") != 0;
                }
                break;
            }
            case 's':
            {
                swap_file = optarg;
                break;
            }
            case 'h':
            default:
                fprintf(stderr, "Usage: %s [-c sigchld/powerbutton_action -s swap_dev] handoff_path", argv[0]);
                return opt != 'h';
        }
    }
    if (optind >= argc)
    {
        fprintf(stderr, "Usage: %s handoff_path", argv[0]);
        return 1;
    }
    // if (strcasecmp(sigchld_action, "ignore") != 0)
    if (0)
        signal(SIGCHLD, sigchld_handler);
    if (swap_file)
    {
        printf("init: Switching swap to %s\n", swap_file);
        obos_status st = (obos_status)syscall1(Sys_SwitchSwap, swap_file);
        if (obos_is_error(st))
        {
            errno = parse_file_status(st);
            perror("Could not switch swap");
        }
    }
    handoff_process = argv[optind];

    setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);

    int ret = print_motd();
    if (ret != 0)
        return ret;

    nm_initialize_hostname();
    nm_initialize_interfaces("/etc/interfaces.json");

    // Start a shell, I guess.
    pid_t pid = fork();
    if (pid == 0)
    {
        while (tcgetpgrp(0) != getpgid(0))
            syscall0(Sys_Yield);
        execvp(handoff_process, &argv[optind]);
        perror("execvp");
        exit(EXIT_FAILURE);
    }
    else 
    {
        tcsetpgrp(0, getpgid(pid));
    }
    int power_button = open("/dev/power_button", O_RDONLY);
    if (power_button != -1 && fork() == 0)
    {
        // ACPI Event Handler.
        is_power_button_handler = true;
        fd_set set = {};
        FD_ZERO(&set);
        FD_SET(power_button, &set);
        select(power_button+1, &set, NULL, NULL, NULL);
        syscall1(Sys_LibCLog, "init: Received power button event\n");
        sigchld_handler(SIGCHLD);
    }
    int status;
    top:
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status))
        printf("Child exitted due to signal %d\n", WTERMSIG(status));
    if (!WIFEXITED(status) && !WIFSIGNALED(status))
        goto top;
    sigchld_handler(SIGCHLD);
    while (1)
        asm volatile ("" :::"memory");
}
