/*
 * init/main.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <unistd.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <strings.h>

#include <sys/wait.h>

#include <obos/syscall.h>

int print_motd();

const char* sigchld_action = "shutdown";

// NOTE: This might need to be changed if init starts "adopting" processes when their parents die (as of this commit, the kernel adopts them).
void sigchld_handler(int num)
{
    (void)(num);
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
    int ret = print_motd();
    if (ret != 0)
        return ret;
    char* handoff_process = NULL;
    int opt = 0;
    while ((opt = getopt(argc, argv, "c:h")) != -1)
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
            case 'h':
            default:
                fprintf(stderr, "Usage: %s [-c sigchld_action] handoff_path", argv[0]);
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
    handoff_process = argv[optind];
    // Start a shell, I guess.
    pid_t pid = fork();
    if (pid == 0)
    {
        while (tcgetpgrp(0) != getpgid(0))
            syscall0(Sys_Yield);
        execvp(handoff_process, &argv[optind]);
        perror("execlp");
        exit(EXIT_FAILURE);
    }
    else 
    {
        tcsetpgrp(0, getpgid(pid));
    }
    int status;
    top:
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status))
        printf("Child exitted due to signal %d\n", WTERMSIG(status));
    if (!WIFEXITED(status) && !WIFSIGNALED(status))
        goto top;
    sigchld_handler(SIGCHLD);
    abort();
    // while (1)
    //     asm volatile ("" :::"memory");
}
