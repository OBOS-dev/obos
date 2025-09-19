#include <obos/syscall.h>
#include <obos/error.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        handle curr = HANDLE_INVALID;
        do {
            char name[64] = {};
            size_t name_size = 64;
            handle old = curr;
            curr = syscall1(Sys_EnumerateLoadedDrivers, old);
            if (old != HANDLE_INVALID)
                syscall1(Sys_HandleClose, old);
            obos_status status = syscall3(Sys_QueryDriverName, curr, name, &name_size);
            if (obos_is_error(status))
            {
                if (curr == HANDLE_INVALID)
                    break;
                else
                {
                    fprintf(stderr, "Sys_QueryDriverName: %d\n", status);
                    return -1;
                }
            }
            printf("%.*s\n", (int)name_size, name);
        } while(curr != HANDLE_INVALID);
    }
    else
    {
        const char* target_name = argv[1];
        handle hnd = syscall1(Sys_FindDriverByName, target_name);
        if (hnd != HANDLE_INVALID)
        {
            printf("%s\n", target_name);
            syscall1(Sys_HandleClose, hnd);
        }
        else
            return 2;
    }
    return 0;
}
