; oboskrnl/arch/x86_64/memmanip.asm
;
; Copyright (c) 2024 Omar Berrow

[BITS 64]

global memset:function default
global memzero:function default
global memcpy:function default
global memcmp:function default
global memcmp_b:function default
global strcmp:function default
global strlen:function default
global strchr:function default

section .text

memset:
	push rbp
	mov rbp, rsp

	push rdi
	mov al, sil
	mov rcx, rdx
	rep stosb
	pop rax

	leave
	ret
memzero:
	push rbp
	mov rbp, rsp

	push rdi
	xor eax, eax
	mov rcx, rsi
	rep stosb
	pop rax

	leave
	ret
memcpy:
	push rbp
	mov rbp, rsp

	mov rax, rdi
	mov rcx, rdx
	rep movsb

	leave
	ret
memcmp:
	push rbp
	mov rbp, rsp

	mov rcx, rdx
	repe cmpsb
	sete al

	leave
	ret
memcmp_b:
	push rbp
	mov rbp, rsp

	mov al, sil
	mov rcx, rdx
	repe scasb
	sete al

	leave
	ret
strcmp:
	push rbp
	mov rbp, rsp
	sub rsp, 0x8

	push rdi
	push rsi
	call strlen
	mov [rbp-8], rax
	mov rdi, rsi
	call strlen
	pop rsi
	pop rdi
	cmp rax, [rbp-8]
	sete al
	jne .end

	mov rdx, [rbp-8]
	call memcmp

.end:
	leave
	ret
strlen:
	push rbp
	mov rbp, rsp

	xor rcx, rcx
	not rcx
	xor eax,eax
	repne scasb
	sub rax, rcx
	sub rax, 2

	leave
	ret
strchr:
	push rbp
	mov rbp, rsp

	push rdi
	call strlen
	pop rdi
	mov r8, rax
	cld
	mov rcx, rax
	mov rax, rsi
	repne scasb
	sub r8, rcx
	mov rax, r8

	leave
	ret
