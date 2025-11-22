; oboskrnl/arch/x86_64/entry.asm

; Copyright (c) 2024 Omar Berrow

[BITS 64]
[DEFAULT ABS]

default rel

; Bootstrap code to initialize the stack guard.

extern __stack_chk_guard
extern Arch_disablePIC
extern Arch_KernelEntry
global Arch_KernelEntryBootstrap:function hidden
Arch_KernelEntryBootstrap:
%if !OBOS_USE_LIMINE
	cmp rsi, 0x554c5442
	je .ok ; should triple fault on failure
	xor rax,rax
	jmp rax ; All hope is lost.
%endif
.ok:
	push rdi
	push rsi
; Get a hardware-generated random value.
; If both methods are unsupported, it will fallback to using the default value used.
 	mov rcx, 10
 .rdrand:
 	mov eax, 1
 	xor ecx,ecx
 	cpuid
 	bt ecx, 30
 	jnc .rdseed
 	rdrand rax
 	jc .move
 	loop .rdrand
 	mov rcx, 10
 .rdseed:
 	mov eax, 7
 	xor ecx,ecx
 	cpuid
 	bt ebx, 18
 	jnc .done
 	rdseed rax
 	jc .move
 	loop .rdseed
 	test rcx, rcx
 	jz .done
 .move:
 	; Move the (likely) random value into the stack guard variable.
 	mov [__stack_chk_guard], rax
 .done:
 	call Arch_disablePIC
 	; Turn on cr0.WP (write protect)
	mov rax, cr0
	or rax, (1<<16) ; WP
	mov cr0, rax
	; Restore rdi and rsi
	pop rsi
	pop rdi
	; Call into the kernel entry.
	push 0 ; Make sure if the kernel entry returns, it triple faults and doesn't do goofy things.
	jmp Arch_KernelEntry
global Arch_IdleTask:function hidden
section .data
global Arch_MakeIdleTaskSleep:data hidden
Arch_MakeIdleTaskSleep: db 0
section .text
Arch_IdleTask:
	hlt
	cmp byte [Arch_MakeIdleTaskSleep], 1
	jne Arch_IdleTask
	cli
.slp:
	pause
	cmp byte [Arch_MakeIdleTaskSleep], 1
	je .slp
	sti
	jmp Arch_IdleTask
