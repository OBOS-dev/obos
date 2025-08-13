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
extern Arch_LogSyscall
extern Arch_LogSyscallRet

section .data
global Arch_cpu_local_currentKernelStack_offset
Arch_cpu_local_currentKernelStack_offset:
    dq 0
section .text

global Arch_SyscallTrapHandler

; NOTE: Clobbers r10, rcx, and r11
; Return value in rax
; Parameter registers (in order):
; rdi, rsi, rdx, r8, r9
; Syscall number is in eax.
Arch_SyscallTrapHandler:
    swapgs
    
    mov r10, [Arch_KernelCR3]
    mov cr3, r10

    mov r10, rsp
    mov rsp, [Arch_cpu_local_currentKernelStack_offset]
    mov rsp, [gs:rsp]
    add rsp, 0x10000

; Only save SysV callee-saved registers, rcx (return address), r10 (return stack), and r11 (return eflags)
    push rcx
    push r10
    push r11
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Use r11 as the syscall table address
    mov r11, OBOS_SyscallTable
    cmp eax, 0x80000000
    jb .not_arch_syscall

    mov r11, OBOS_ArchSyscallTable
    sub eax, 0x80000000

.not_arch_syscall:

    ; Conserve caller-saved "scratch" registers before logging the syscall
    push rax
    push rdi
    push rsi
    push rdx
    push rcx
    push r8
    push r9

    mov rcx, r8
    mov r8, r9
    mov r9, rax
    call Arch_LogSyscall

    pop r9
    pop r8
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rax

    push rax

    mov rcx, r8
    mov r8, r9
    call [r11+rax*8]
    
    pop rsi
    push rax
    mov rdi, rax
    call Arch_LogSyscallRet
    pop rax

    mov r9, gs:0x18
    mov r9, [r9] ; currentContext->pt
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    pop r11
    pop r10
    pop rcx
    mov cr3, r9

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

