#include <obos/syscall.h>
#include <unistd.h>
#include <strings.h>
#include <stdio.h>

static char obos_getchar()
{
    char ch[2] = {};
    syscall4(Sys_FdRead, 0, &ch, 2, NULL);
    return ch[0];
}
#define confirm() do \
{\
    fputs("Continue? y/n ", stderr);\
    char c = 0;\
    do {\
        c = obos_getchar();\
        switch (c)\
        {\
            case 'y': break;\
            case 'n': puts("Abort"); return 1;\
            case '\n': c = 'y'; break;\
            default: fputs("Please put y/n ", stderr); break;\
        }\
    } while(c != 'y');\
} while(0)

int main(int argc, char** argv)
{
    const char* option = argv[1] ? argv[1] : "shutdown";
    if (strcasecmp(option, "suspend") == 0)
    {
        puts("Suspending...");
        confirm();
        syscall0(Sys_Suspend);
    }
    else if (strcasecmp(option, "reboot") == 0)
    {
        puts("Rebooting...");
        confirm();
        syscall0(Sys_Reboot);
    }
    else
    {
        puts("Shutting down...");
        confirm();
        syscall0(Sys_Shutdown);
    }
    return 0;
}
