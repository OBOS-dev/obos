/*
 * init/motd.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <stdio.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

int print_motd()
{
    struct stat st = {};
    if (stat("/etc/motd", &st) != 0)
    {
        perror("stat(\"/etc/motd\")");
        return -1;
    }
    int motd_fd = open("/etc/motd", O_RDONLY);
    if (motd_fd < 0)
    {
        perror("open(\"/etc/motd\", O_RDONLY)");
        return -1;
    }
    void* motd = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, motd_fd, 0);
    write(STDOUT_FILENO, motd, st.st_size);
    write(STDOUT_FILENO, "\n", 1);
    close(motd_fd);
    munmap(motd, st.st_size);
    return 0;
}