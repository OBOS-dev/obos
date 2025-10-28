// Loads a driver, potentially starting it.

#include <obos/syscall.h>
#include <obos/error.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Usage: %s pathspec [boolean: start]\n", argv[0]);
        return -1;
    }

    bool start_driver = true;
    if (argc >= 3)
    {
        if (strcasecmp(argv[2], "true") == 0)
            start_driver = true;
        else if (strcasecmp(argv[2], "false") == 0)
            start_driver = false;
        else
        {
            errno = 0;
            int i_val = strtol(argv[2], NULL, 0);
            if (errno != 0)
            {
                fprintf(stderr, "Expected boolean, got: %s\n", argv[2]);
                return -1;
            }
            start_driver = !!i_val;
        }
        
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd == -1)
    {
        fprintf(stderr, "open(%s): %s", argv[1], strerror(errno));
        return -1;
    }
    
    struct stat st = {};
    fstat(fd, &st);

    size_t sz_buf = st.st_size;
    void* buf = mmap(NULL, sz_buf, PROT_READ, MAP_PRIVATE, fd, 0);
    if (!buf)
    {
        perror("mmap");
        return -1;
    }

    obos_status status = OBOS_STATUS_SUCCESS;
    printf("Loading driver at %s\n", argv[1]);
    handle hnd = syscall3(Sys_LoadDriver, buf, sz_buf, &status);
    if (obos_is_error(status))
    {
        fprintf(stderr, "Sys_LoadDriver: %d\n", status);
        return -1;
    }
    char name_buf[64];
    size_t sz_name_buf = sizeof(name_buf);
    syscall3(Sys_QueryDriverName, hnd, name_buf, &sz_name_buf);
    printf("Loaded driver '%.*s' at %s\n", (int)sz_name_buf, name_buf, argv[1]);

    if (start_driver)
    {
        printf("Starting driver '%.*s'\n", (int)sz_name_buf, name_buf);
        status = syscall2(Sys_StartDriver, hnd, NULL);
        if (obos_is_error(status))
        {
            fprintf(stderr, "Sys_StartDriver: %d\n", status);
            return -1;
        }
        printf("Started driver '%.*s'\n", (int)sz_name_buf, name_buf);
    }
    
    return 0;
}
