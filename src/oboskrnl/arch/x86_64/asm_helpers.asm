; oboskrnl/arch/x86_64/asm_helpers.asm
;
; Copyright (c) 2024 Omar Berrow

[BITS 64]

global _ZN4obos6getCR0Ev
global _ZN4obos6getCR2Ev
global _ZN4obos6getCR3Ev
global _ZN4obos6getCR4Ev
global _ZN4obos6getCR8Ev
global _ZN4obos5pauseEv
global _ZN4obos7getEFEREv
global _ZN4obos5rdmsrEj
global _ZN4obos5wrmsrEjm

_ZN4obos6getCR0Ev:
	push rbp
	mov rbp, rsp

	mov rax, cr0

	leave
	ret
_ZN4obos6getCR2Ev:
	push rbp
	mov rbp, rsp	

	mov rax, cr2
	
	leave
	ret
_ZN4obos6getCR3Ev:
	push rbp
	mov rbp, rsp	

	mov rax, cr3
	
	leave
	ret
_ZN4obos6getCR4Ev:
	push rbp
	mov rbp, rsp

	mov rax, cr4
	
	leave
	ret
_ZN4obos6getCR8Ev:
	push rbp
	mov rbp, rsp

	mov rax, cr8

	leave
	ret
_ZN4obos7getEFEREv:
	push rbp
	mov rbp, rsp

	mov ecx, 0xc0000080 ; EFER
	rdmsr
	shl rdx, 32
	or rax, rdx
	
	leave
	ret
_ZN4obos5rdmsrEj:
	push rbp
	mov rbp, rsp

	mov ecx, edi
	rdmsr
	shl rdx, 32
	or rax, rdx

	leave
	ret
_ZN4obos5wrmsrEjm:
	push rbp
	mov rbp, rsp

	mov ecx, edi
	mov eax, esi
	mov rdx, rsi
	shr rdx, 32
	wrmsr
	
	leave
	ret
_ZN4obos5pauseEv:
	pause
	ret