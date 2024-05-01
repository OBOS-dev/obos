; oboskrnl/arch/x86_64/memmanip.asm
;
; Copyright (c) 2024 Omar Berrow

[BITS 64]

global memset
global memzero
global memcpy
global memcmp
global memcmp_b
global strcmp
global strlen

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
	xor rax,rax
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
	mov rcx, rax
	mov rax, rsi
	repne scasb
	mov rax, rdi

	leave
	ret