; oboskrnl/arch/x86_64/syscall.asm
;
; Copyright (c) 2024 Omar Berrow

BITS 64

extern OBOS_SyscallTable
extern OBOS_ArchSyscallTable
extern Core_RaiseIrql
extern Core_LowerIrql
extern Arch_KernelCR3

%macro sys_pushaq 0
push rcx
push rbx
push rsi
push rdi
push r8
push r9
push r10
push r11
push r12
push r13
push r14
push r15
push rcx ; rip
push rbp
%endmacro

%macro sys_popaq 0
pop rbp
add rsp, 8
pop r15
pop r14
pop r13
pop r12
pop r11
pop r10
pop r9
pop r8
pop rdi
pop rsi
pop rbx
pop rcx
%endmacro

global Arch_SyscallTrapHandler
; NOTE: Clobbers r10
Arch_SyscallTrapHandler:
    swapgs
    mov r10, rsp
    mov rsp, [gs:0xc8]
    add rsp, 0x30000
    sys_pushaq
    mov rbp, rsp

    mov rdx, cr3
    push rdx
    mov rdx, [Arch_KernelCR3]
    mov cr3, rdx

    ; eax has the syscall number.

    mov r11, OBOS_SyscallTable

    cmp eax, 0x80000000
    jb .not_arch_syscall

; An arch-specific syscall.
    mov r11, OBOS_ArchSyscallTable
    sub eax, 0x80000000

.not_arch_syscall:

    push rax
    push rdi
    push rsi
    push rdx
    push rcx
    push r8
    push r9
    mov rdi, 0x2 ; IRQL_DISPATCH
    call Core_RaiseIrql
    sti
    pop r9
    pop r8
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop r10
    push rax
    mov rax, r10

    mov r10, 0xffffffff
    and rax, r10
    call [r11+rax*8]

    cli
    pop rdi
    push rax
    push rdx
    call Core_LowerIrql
    pop rdx
    pop rax

.done:
    pop r9
    mov cr3, r9

    mov rsp, rbp
    sys_popaq
    mov rsp, r10
    swapgs
    o64 sysret
global Arch_SyscallTrapHandlerEnd
Arch_SyscallTrapHandlerEnd:
