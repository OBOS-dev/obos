/*
 * init/main.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <stdint.h>
#include <stddef.h>

#include "syscall.h"

void _start()
{
    syscall0(Sys_Shutdown);
    syscall0(Sys_ExitCurrentThread);
}

#if defined(__x86_64__)
asm (".intel_syntax noprefix;"
".global syscall;"
"syscall:;"
"   push rbp;"
"   mov rbp, rsp;"

"   mov eax, edi;"
"   mov rdi, rsi;"
"   mov rsi, rdx;"
"   mov rdx, rcx;"
"   mov r8, r9;"
"   mov r9, [rbp+0x8];"
"   syscall;"

"   leave;"
"   ret;"
".att_syntax prefix"
);
#endif
