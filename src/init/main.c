/*
 * init/main.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <unistd.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

int print_motd();

int main(int argc, char** argv)
{
    if (getpid() != 1)
        return -1;
    int ret = print_motd();
    if (ret != 0)
        return ret;
    char* handoff_process = NULL;
    int opt = 0;
    while ((opt = getopt(argc, argv, "h")) != -1)
    {
        switch (opt)
        {
            default:
                fprintf(stderr, "Usage: %s handoff_path", argv[0]);
                return 1;
        }
    }
    if (optind >= argc)
    {
        fprintf(stderr, "Usage: %s handoff_path", argv[0]);
        return 1;
    }
    handoff_process = argv[optind];
    // Start a shell, I guess.
    if (fork() == 0)
    {
        execlp(handoff_process, "");
        perror("execlp");
        exit(EXIT_FAILURE);
    }
    while (1)
        asm volatile ("" :::"memory");
}