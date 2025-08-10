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

; NOTE: Clobbers r10,rcx,r11,rdx
; Return value in rax
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
    push rax
    push .finished
    jz Sys_InvalidSyscall
extern Arch_LogSyscall
extern Arch_LogSyscallRet
push rdi
push rsi
push rdx
push rcx
push r8
push r9
push rax
push r11
    mov r9, rax
    call Arch_LogSyscall
pop r11
pop rax
pop r9
pop r8
pop rcx
pop rdx
pop rsi
pop rdi

    call [r11+rax*8]
.finished:
    cli

    pop rsi
    push rax
    push rdx

    mov rdi, rax
    call Arch_LogSyscallRet

    pop rdx
    pop rax

    add rsp, 8

    mov r9, gs:0x18
    mov r9, [r9]
    mov [rsp+14*8], r9 ; old_cr3 = currentContext->pt
    sys_popaq

    xor rdx,rdx
    mov rsp, r10
    swapgs
    o64 sysret
global Arch_SyscallTrapHandlerEnd
; OBOS_NORETURN void Arch_GotoUser(uintptr_t rip, uintptr_t cr3, uintptr_t rsp);
global Arch_GotoUser
extern Core_GetIRQLVar
Arch_GotoUser:
    cli

    mov rax, 0
	mov cr8, rax
	push rdi
	push rsi
	push rdx
	call Core_GetIRQLVar
	pop rdx
	pop rsi
	pop rdi
	mov qword [rax], 0

	mov cr3, rsi

	swapgs

    mov rcx, rdi
    mov r11, 0x200202 ; rflags=IF,ID
    mov rsp, rdx
    o64 sysret
Arch_SyscallTrapHandlerEnd:

