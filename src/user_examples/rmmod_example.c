// Starts an already loaded module.

#include <obos/syscall.h>
#include <obos/error.h>
#include <stdio.h>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Usage: %s driver_name\n", argv[0]);
        return -1;
    }

    handle hnd = syscall1(Sys_FindDriverByName, argv[1]);
    if (hnd != HANDLE_INVALID)
    {
        fprintf(stderr, "Could not find driver %s\n", argv[1]);
        return -1;
    }

    printf("Unloading driver '%s'\n", argv[1]);
    obos_status status = syscall1(Sys_UnloadDriver, hnd);
    if (obos_is_error(status))
    {
        fprintf(stderr, "Sys_UnloadDriver: %d\n", status);
        return -1;
    }
    printf("Unloaded driver '%s'\n", argv[1]);
    
    return 0;
}
