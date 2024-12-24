; oboskrnl/arch/x86_64/syscall.asm
;
; Copyright (c) 2024 Omar Berrow

BITS 64

extern OBOS_SyscallTable
extern OBOS_ArchSyscallTable
extern Core_RaiseIrql
extern Core_LowerIrql
extern Arch_KernelCR3
extern Sys_InvalidSyscall

%macro sys_pushaq 0
push 0 ; cr3
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
pop r9 ; cr3
mov cr3, r9
%endmacro

section .data
global Arch_cpu_local_currentKernelStack_offset
Arch_cpu_local_currentKernelStack_offset:
    dq 0
section .text

global Arch_SyscallTrapHandler

; NOTE: Clobbers r10,rcx,r11
; Return value in rdx:rax
; Parameter registers (in order):
; rdi, rsi, rdx, r8, r9
; Syscall number is in eax.
Arch_SyscallTrapHandler:
    swapgs
    mov r10, rsp
    ; Switch to a temporary stack
    mov rsp, [gs:0xc8]
    add rsp, 0x20000

    push rdx
    mov rdx, [Arch_KernelCR3]
    mov cr3, rdx
    pop rdx

    ; Switch to the thread's 'proper' stack
    mov rsp, [Arch_cpu_local_currentKernelStack_offset] ; hacky, but works
    mov rsp, [gs:rsp]
    add rsp, 0x10000
    sys_pushaq

    ; eax has the syscall number.

    mov r11, OBOS_SyscallTable

    cmp eax, 0x80000000
    jb .not_arch_syscall

; An arch-specific syscall.
    mov r11, OBOS_ArchSyscallTable
    sub eax, 0x80000000

.not_arch_syscall:

    sti

    mov r10, 0xffffffff
    and rax, r10
    mov rcx, r8
    mov r8, r9
    cmp qword [r11+rax*8], 0
    ; Basically a call if zero
    ; Maybe we should just do something normal?
    push .finished
    jz Sys_InvalidSyscall
    call [r11+rax*8]
    add rsp, 8
.finished:
    cli

    mov r9, gs:0x18
    mov r9, [r9]
    mov [rsp+14*8], r9 ; old_cr3 = currentContext->pt
    sys_popaq

    mov rsp, r10
    swapgs
    o64 sysret
global Arch_SyscallTrapHandlerEnd
Arch_SyscallTrapHandlerEnd:
