#include <obos/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

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

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("%s level", argv[0]);
        return -1;
    }
    int level = strtol(argv[1], NULL, 0);
    if (level == LONG_MAX)
    {
        perror("strtol");
        return -1;
    }
    if (level < 0 || level > 4)
    {
        errno = EINVAL;
        perror("Sys_SetKLogLevel");
        return -1;
    }
    syscall1(Sys_SetKLogLevel, level);
    return 0;
}
