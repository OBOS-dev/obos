/*
 * init/main.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "syscall.h"
#include "allocator.h"

void* init_mmap(size_t sz)
{
    struct
    {
        uint32_t prot;
        uint32_t flags;
        handle file;
    } extra_args = {0,0,HANDLE_INVALID};
    return (void*)syscall5(Sys_VirtualMemoryAlloc, HANDLE_CURRENT, NULL, sz, &extra_args, NULL);
}

void init_munmap(void* blk, size_t sz)
{
    syscall3(Sys_VirtualMemoryFree, HANDLE_CURRENT, blk, sz);
}

extern size_t init_pgsize()
{
    return OBOS_PAGE_SIZE;
}

__attribute__((no_sanitize("address")))
void *memcpy(void* dest_, const void* src_, size_t sz)
{
    char* dest = dest_;
    const char* src = src_;
    for (size_t i = 0; i < sz; i++)
        dest[i] = src[i];
    return dest_;
}

__attribute__((no_sanitize("address")))
void *memset(void* dest_, int a, size_t sz)
{
    char* dest = dest_;
    for (size_t i = 0; i < sz; i++)
        dest[i] = a;
    return dest_;
}

uint64_t random_number();
uint8_t random_number8();
__asm__(
    "random_number:; rdrand %rax; ret; "
    "random_number8:; rdrand %ax; mov $0, %ah; ret; "
);

size_t nAllocations;
static void test_allocator()
{
    void* to_free = NULL;
    size_t free_size = 0;
    while (true)
    {
        nAllocations++;
        size_t sz = random_number() % 2048 + 8;
        char* ret = malloc(sz);
        ret[0] = random_number8();
        ret[sz-1] = random_number8();
        if (random_number() % 2)
        {
            free(to_free, free_size);
            to_free = ret;
            free_size = sz;
        }
    }
}

typedef struct siginfo_t
{
    int signum;
    int sigcode;
    struct thread* sender;
    void* addr;
    int status;
    union {
        void* ptr;
        uintptr_t integer;
    } udata;
} siginfo_t;

void sigsegv(int signum, siginfo_t* info, void* wut)
{
    (void)(signum);
    (void)(wut);
    (void)(info);
    siginfo_t* volatile info_ = info;
    (void)(info_);
    syscall0(Sys_Yield);
    syscall0(Sys_ExitCurrentThread);
}

typedef struct user_sigaction {
    union {
        void(*handler)(int signum);
        void(*sa_sigaction)(int signum, /*siginfo_t*/siginfo_t* info, void* unknown);
    } un;
    // NOTE(oberrow): Set to __mlibc_restorer in the mlibc sysdeps.
    uintptr_t trampoline_base; // required
    uint32_t  flags;
} user_sigaction;

void _start()
{
    user_sigaction sig = {.flags=1<<0,.un.sa_sigaction=sigsegv,.trampoline_base=0};
    syscall3(Sys_SigAction, 11 /*SIGSEGV*/, &sig, NULL);
    test_allocator();
    syscall0(Sys_ExitCurrentThread);
}

#if defined(__x86_64__)
asm (".intel_syntax noprefix;"
".global syscall;"
"syscall:;"
"   push rbp;"
"   mov rbp, rsp;"

"   mov eax, edi;"
"   mov rdi, rsi;" // arg0 (rsi) -> syscall arg0 (rdi)
"   mov rsi, rdx;" // arg1 (rdx) -> syscall arg1 (rsi)
"   mov rdx, rcx;" // arg2 (rcx) -> syscall arg2 (rdx)
// "   mov r8, r8;"   // arg3 (r8) -> syscall arg3 (r8)
// "   mov r9, r9;"   // arg4 (r9) -> syscall arg4 (r9)
"   syscall;"

"   leave;"
"   ret;"
".att_syntax prefix"
);
#endif
