/*
 * init/main.c
 *
 * Copyright (c) 2025-2026 Omar Berrow
*/

#include <unistd.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <utmp.h>
#include <utmpx.h>
#include <strings.h>
#include <string.h>
#include <paths.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

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

    setutxent();

    do {
        struct utmpx dead_proc_entry = {};
        struct timespec ts;
        clock_gettime(0, &ts);
    
        dead_proc_entry.ut_tv.tv_sec = ts.tv_sec;
        dead_proc_entry.ut_tv.tv_usec = ts.tv_nsec/1000;
        dead_proc_entry.ut_type = DEAD_PROCESS;
        dead_proc_entry.ut_pid = 1;
        
        char* tty_name = ttyname(STDIN_FILENO);
        if (tty_name)
        {
            strncpy(dead_proc_entry.ut_line, tty_name + 5 /* remove /dev/ */, sizeof(dead_proc_entry.ut_line) - 1);
            strncpy(dead_proc_entry.ut_id, tty_name + strlen(tty_name) - 4, sizeof(dead_proc_entry.ut_id) - 1);
        }
        
        if (pututxline(&dead_proc_entry) == NULL)
            perror("pututxline");
        updwtmpx(_PATH_WTMP, &dead_proc_entry);
    } while(0);

    endutxent();

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
    if (getuid() != 0)
        return -1;

    const char* swap_file = NULL;
    char* handoff_process = NULL;
    int opt = 0;
    long final_log_level = 2;
    while ((opt = getopt(argc, argv, "+s:c:l:h")) != -1)
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
            case 'l':
            {
                errno = 0;
                final_log_level = strtol(optarg, NULL, 0);
                if (errno != 0 || final_log_level < 0 || final_log_level > 4)
                {
                    fprintf(stderr, "Expected integer within [0...4] for -l option, got %s instead\n", optarg);
                    return -1;
                }
                break;
            }
            case 'h':
            default:
                fprintf(stderr, "Usage: %s [-c sigchld/powerbutton_action] [-s swap_dev] [-l kernel_log_level] handoff_path [handoff program arguments]", argv[0]);
                return opt != 'h';
        }
    }
    if (optind >= argc)
    {
        fprintf(stderr, "Usage: %s [-c sigchld/powerbutton_action] [-s swap_dev] [-l kernel_log_level] handoff_path [handoff program arguments]", argv[0]);
        return 1;
    }
    // if (strcasecmp(sigchld_action, "ignore") != 0)

    struct timespec ts = {};
    clock_gettime(0, &ts);
    
    setutxent();
    do {
        struct utmpx boot_entry = {};
    
        boot_entry.ut_tv.tv_sec = ts.tv_sec;
        boot_entry.ut_tv.tv_usec = ts.tv_nsec/1000;
        boot_entry.ut_type = BOOT_TIME;
        boot_entry.ut_pid = 1;
        memcpy(boot_entry.ut_line, "reboot", 6);
        
        if (pututxline(&boot_entry) == NULL)
            perror("pututxline");
        updwtmpx(_PATH_WTMP, &boot_entry);
    } while(0);
    do {
        struct utmpx init_proc_entry = {};
        struct timespec ts;
    
        clock_gettime(0, &ts);
    
        init_proc_entry.ut_tv.tv_sec = ts.tv_sec;
        init_proc_entry.ut_tv.tv_usec = ts.tv_nsec/1000;
        init_proc_entry.ut_type = INIT_PROCESS;
        init_proc_entry.ut_pid = 1;

        char* tty_name = ttyname(STDIN_FILENO);
        if (tty_name)
        {
            strncpy(init_proc_entry.ut_line, tty_name + 5 /* remove /dev/ */, sizeof(init_proc_entry.ut_line) - 1);
            strncpy(init_proc_entry.ut_id, tty_name + strlen(tty_name) - 4, sizeof(init_proc_entry.ut_id) - 1);
        }
        
        if (pututxline(&init_proc_entry) == NULL)
            perror("pututxline");
        updwtmpx(_PATH_WTMP, &init_proc_entry);
    } while(0);
    endutxent();

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

    setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);

    nm_initialize_hostname();
    nm_initialize_interfaces("/etc/interfaces.json");

    int ret = print_motd();
    if (ret != 0)
        return ret;

    syscall1(Sys_SetKLogLevel, final_log_level);

    // Start a shell, I guess.
    pid_t pid = fork();
    if (pid == 0)
    {
        setpgrp();
        tcsetpgrp(0, getpgrp());
        execvp(handoff_process, &argv[optind]);
        perror("execvp");
        exit(EXIT_FAILURE);
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
        printf("Handoff process exited due to signal %d\n", WTERMSIG(status));
    if (!WIFEXITED(status) && !WIFSIGNALED(status))
        goto top;
    sigchld_handler(SIGCHLD);
    while (1)
        asm volatile ("" :::"memory");
}
