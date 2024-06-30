; oboskrnl/arch/x86_64/bgdt.asm

; Copyright (c) 2024 Omar Berrow

[BITS 64]

section .no.mm.bss
Arch_InitialISTStack:
	resb 0x10000
global Arch_InitialISTStack
section .data
align 1
TSS:
	.rsv1: dd 0
	.rsp0: dq 0
	.rsp1: dq 0
	.rsp2: dq 0
	.rsv2: dq 0
	.ist0: dq 0
	.ist1: dq 0
	.ist2: dq 0
	.ist3: dq 0
	.ist4: dq 0
	.ist5: dq 0
	.ist6: dq 0
	.ist7: dq 0
	.rsv3: dq 0
	.rsv4: dw 0
	.iopb: dw 103
.end:
TSS_Len equ TSS.end-TSS
global GDT
GDT:
	.null: dq 0
	.kcode: dq 0x00af9b000000ffff
	.kdata: dq 0x00af93000000ffff
	.tss_limitLow: dw 0
	.tss_baseLow: dw 0
	.tss_baseMiddle1: db 0
	.tss_access: db 0x89
	.tss_gran: db 0x40
	.tss_baseMiddle2: db 0
	.tss_baseHigh: dd 0
	.tss_resv1: dd 0
.end:
GDTPtr:
	.len: dw GDT.end-GDT-1
	.base: dq GDT
section .text

global Arch_InitBootGDT

Arch_InitBootGDT:
	push rbp
	mov rbp, rsp
	sub rsp, 10

	mov rax, TSS
	mov word [GDT.tss_limitLow], TSS_Len
	mov [GDT.tss_baseLow], ax
	mov rdx, rax
	shr rdx, 16
	mov [GDT.tss_baseMiddle1], dl
	shr rdx, 8
	mov [GDT.tss_baseMiddle2], dl
	shr rdx, 8
	mov [GDT.tss_baseHigh], edx

	lea rax, [Arch_InitialISTStack+0x10000]
	mov [TSS.ist0], rax

	lgdt [GDTPtr]

	mov ax, 0x18
	ltr ax

	mov ax, 0x10
	mov ds, ax
	mov ss, ax
	mov es, ax
	mov fs, ax
	mov gs, ax

	leave
	pop rax
	push 0x8
	push rax
	retfq
global Arch_FlushGDT
Arch_FlushGDT:
	push rbp
	mov rbp, rsp

	lgdt [rdi]

	mov ax, 0x28
	ltr ax

	mov ax, 0x10
	mov ds, ax
	mov ss, ax
	mov es, ax
	mov fs, ax
	mov gs, ax 

	leave
	pop rax
	push 0x8
	push rax
	retfq