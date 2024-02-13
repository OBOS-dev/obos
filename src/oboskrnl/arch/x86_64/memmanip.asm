; oboskrnl/arch/x86_64/memmanip.asm
;
; Copyright (c) 2024 Omar Berrow

[BITS 64]

global _ZN4obos6memsetEPvjm
global _ZN4obos7memzeroEPvm
global _ZN4obos6memcpyEPvPKvm
global _ZN4obos6memcmpEPKvS1_m
global _ZN4obos6strcmpEPKcS1_
global _ZN4obos6strlenEPKc

_ZN4obos6memsetEPvjm:
	push rbp
	mov rbp, rsp

	push rdi
	mov al, sil
	mov rcx, rdx
	rep stosb
	pop rax

	leave
	ret
_ZN4obos7memzeroEPvm:
	push rbp
	mov rbp, rsp

	push rdi
	xor rax, rax
	mov rcx, rdx
	rep stosb
	pop rax

	leave
	ret
_ZN4obos6memcpyEPvPKvm:
	push rbp
	mov rbp, rsp

	mov rax, rdi
	mov rcx, rdx
	rep movsb

	leave
	ret
_ZN4obos6memcmpEPKvS1_m:
	push rbp
	mov rbp, rsp

	mov rcx, rdx
	repne cmpsb
	setne al

	leave
	ret
_ZN4obos6strcmpEPKcS1_:
	push rbp
	mov rbp, rsp
	sub rsp, 0x8

	call _ZN4obos6strlenEPKc
	mov [rbp-8], rax
	push rdi
	mov rdi, rsp
	call _ZN4obos6strlenEPKc
	pop rdi
	cmp rax, [rbp-8]
	setne al
	jne .end

	mov rdx, [rbp-8]
	call _ZN4obos6memcmpEPKvS1_m

.end:
	leave
	ret
_ZN4obos6strlenEPKc:
	push rbp
	mov rbp, rsp

	xor rcx, rcx
	not rcx
	xor rax,rax
	repne scasb
	sub rax, rcx
	sub rax, 2

	leave
	ret