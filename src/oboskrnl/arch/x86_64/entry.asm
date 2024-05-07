; oboskrnl/arch/x86_64/entry.asm

; Copyright (c) 2024 Omar Berrow

[BITS 64]

; Bootstrap code to initialize the stack guard.

extern __stack_chk_guard
extern Arch_disablePIC
extern Arch_KernelEntry
global Arch_KernelEntryBootstrap
Arch_KernelEntryBootstrap:
	cmp rsi, 0x554c5442
	je .ok ; should triple fault
	xor rax,rax
	jmp rax ; All hope is lost.
.ok:
	push rdi
	push rsi
; Get a hardware-generated random value.
; If both methods are unsupported, it will fallback to using the default value used.
.rdrand:
	mov eax, 7
	xor ecx,ecx
	cpuid
	test ebx, (1<<18)
	jne .rdseed
	rdrand rax
	jnc .rdrand
	jmp .move
.rdseed:
	mov eax, 1
	xor ecx,ecx
	cpuid
	test ecx, (1<<30)
	jne .done
	rdseed rax
	jnc .rdseed
.move:
	; Move the (likely) random value into the stack guard variable.
	mov [__stack_chk_guard], rax
.done:
	call Arch_disablePIC
	; Restore rdi and rsi
	pop rsi
	pop rdi
	; Call into the kernel entry.
	push 0 ; Make sure if the kernel entry returns, it triple faults and doesn't do goofy things.
	jmp Arch_KernelEntry