; oboskrnl/arch/x86_64/asm_helpers.asm
;
; Copyright (c) 2024 Omar Berrow

[BITS 64]

global _ZN4obos6getCR0Ev
global _ZN4obos6getCR2Ev
global _ZN4obos6getCR3Ev
global _ZN4obos6getCR4Ev
global _ZN4obos6getCR8Ev
global _ZN4obos7getEFEREv

_ZN4obos6getCR0Ev:
	mov rax, cr0
	ret
_ZN4obos6getCR2Ev:
	mov rax, cr2
	ret
_ZN4obos6getCR3Ev:
	mov rax, cr3
	ret
_ZN4obos6getCR4Ev:
	mov rax, cr4
	ret
_ZN4obos6getCR8Ev:
	mov rax, cr8
	ret
_ZN4obos7getEFEREv:
	mov ecx, 0xc0000080 ; EFER
	rdmsr
	shl rdx, 32
	or rax, rdx
	ret