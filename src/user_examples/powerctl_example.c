#include <obos/syscall.h>
#include <unistd.h>
#include <strings.h>
#include <stdio.h>

asm (
".intel_syntax noprefix;\
\
.global syscall;\
\
syscall:;\
    push rbp;\
    mov rbp, rsp;\
\
    mov eax, edi;\
    mov rdi, rsi;\
    mov rsi, rdx;\
    mov rdx, rcx;\
\
    syscall;\
\
    leave;\
    ret;\
\
.hidden syscall;\
\
.att_syntax prefix;");

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
        asm volatile ("syscall;" : :"r"(Sys_Suspend) :"memory");
    }
    else if (strcasecmp(option, "reboot") == 0)
    {
        puts("Rebooting...");
        confirm();
        asm volatile ("syscall;" : :"r"(Sys_Reboot) :"memory");
    }
    else
    {
        puts("Shutting down...");
        confirm();
        asm volatile ("syscall;" : :"r"(Sys_Shutdown) :"memory");
    }
    return 0;
}
