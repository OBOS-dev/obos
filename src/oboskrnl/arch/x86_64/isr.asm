; oboskrnl/arch/x86_64/isr.asm
;
; Copyright (c) 2024 Omar Berrow

bits 64

section .no.pti.text
default rel

align 32
global Arch_b_isr_handler
global Arch_e_isr_handler
global Arch_FlushIDT
Arch_b_isr_handler:
%macro isr_handler_no_ec 1
align 32
isr%1:
	cld
	push 0
	push %1
	push %1-0x20
	jmp int_handler_common
%endmacro
%macro isr_handler_ec 1
align 0x20
isr%1:
	cld
	push %1
	push %1-0x20
	jmp int_handler_common
%endmacro
isr_handler_no_ec  0
isr_handler_no_ec  1
isr_handler_no_ec  2
isr_handler_no_ec  3
isr_handler_no_ec  4
isr_handler_no_ec  5
isr_handler_no_ec  6
isr_handler_no_ec  7
isr_handler_ec     8
isr_handler_no_ec  9
isr_handler_ec    10
isr_handler_ec    11
isr_handler_ec    12
isr_handler_ec    13
isr_handler_ec    14
isr_handler_no_ec 15
isr_handler_no_ec 16
isr_handler_ec    17
isr_handler_no_ec 18
isr_handler_no_ec 19
isr_handler_no_ec 20
isr_handler_ec    21
isr_handler_no_ec 22
isr_handler_no_ec 23
isr_handler_no_ec 24
isr_handler_no_ec 25
isr_handler_no_ec 26
isr_handler_no_ec 27
isr_handler_no_ec 28
isr_handler_no_ec 29
isr_handler_no_ec 30
isr_handler_no_ec 31
%assign current_int_number 32
%rep 224
isr_handler_no_ec current_int_number
%assign current_int_number current_int_number+1
%endrep
Arch_e_isr_handler:

%macro pushaq 0
push rax
; rax has rsp.
mov rax, rsp
add rax, 8
push rcx
push rdx
push rbx
push rax ; Push rsp
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
push qword [rsp+0x90]
push rbp
%endmacro

; Cleans up after pushaq.

%macro popaq 0
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
add rsp, 8
pop rbx
pop rdx
pop rcx
pop rax
%endmacro

extern Arch_IRQHandlers
global Arch_KernelCR3
Arch_KernelCR3:
	dq 0

int_handler_common:
	pushaq
	mov rbp, rsp

	mov rax, ds
	push rax

	mov rax, cr3
	push rax

	test qword [rsp+0xB8], 0x3 ; User code
	je .no_swapgs1
	mov rax, [Arch_KernelCR3]
	mov cr3, rax
	swapgs
.no_swapgs1:
	mov rax, [rsp+0xA0]
	cmp rax, 255
	ja .finished
	mov rax, [Arch_IRQHandlers+rax*8]

	test rax,rax
	jz .finished

	mov rdi, rsp
	call rax

.finished:
	test qword [rsp+0xB8], 0x3 ; User code

	pop rax
	je .no_swapgs2
	mov cr3, rax
	swapgs
.no_swapgs2:

	mov rsp, rbp
	popaq

	add rsp, 0x18

	iretq
section .text
Arch_FlushIDT:
	lidt [rdi]
	ret
global CoreS_GetIRQL
global CoreS_SetIRQL
extern OBOS_Panic
section .rodata
panic_format1: db "Invalid IRQL %d passed to CoreS_GetIRQL.", 0xa, 0x0
section .text
; Takes no registers as input.
; Sets rax to the current IRQL (cr8).
CoreS_GetIRQL:
	push rbp
	mov rbp, rsp
	
	mov rax, cr8
	
	leave
	ret
; Input:
; rdi: New IRQL.
CoreS_SetIRQL:
	push rbp
	mov rbp, rsp

	cmp rdi, 1
	jne .success
; Panic!
	mov rax, 1
	push rdi
	mov rdi, 1 ; OBOS_PANIC_FATAL_ERROR
	mov rsi, panic_format1
	pop rdx
	call OBOS_Panic
.success:
	mov cr8, rdi
	
	leave
	ret
global Arch_disablePIC
Arch_disablePIC:
	mov al, 0xff
	out 0x21, al
	out 0xA1, al
	ret