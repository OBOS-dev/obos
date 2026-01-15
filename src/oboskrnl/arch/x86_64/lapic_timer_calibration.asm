; oboskrnl/arch/x86_64/lapic_timer_calibration.asm
;
; Copyright (c) 2024-2026 Omar Berrow

[BITS 64]
[DEFAULT ABS]

global Arch_FindCounter:function hidden
extern Arch_LAPICAddress
extern Arch_HPETAddress
extern Arch_CalibrateHPET

; uint64_t Arch_FindCounter(uint64_t hz);
; (input)  rdi: The expected frequency of the LAPIC count.
; (output) rax: The timer count for the LAPIC, assuming the divisor is LAPIC_TIMER_DIVISIOR_ONE (0b1101)
Arch_FindCounter:
	push rbp
	mov rbp, rsp
	push r15
	push r13
	
	call Arch_CalibrateHPET
	mov r15, rax
	mov r13, [Arch_LAPICAddress]
	mov r11, [Arch_HPETAddress]

	mov r13, [Arch_LAPICAddress]
	mov dword [r13+0x3E0], 0xB ; DIVISOR_ONE
	mov dword [r13+0x380], 0xffffffff

	; Start the HPET timer.
	mov rax, [r11+0x10]
	or rax, (1<<0)
	mov [r11+0x10], rax

	add r11, 0xf0
	
.loop:
	mov r9, [r11]
	cmp r9, r15
	jnge .loop
	
	xor r9,r9
	mov r9d, [r13+0x390]
	mov dword [r13+0x380], 0
	mov rax, 0xffffffff
	sub rax, r9

.end:
	pop r13
	pop r15
	leave
	ret
extern rdtsc
global Arch_FindTSCChangeRate
Arch_FindTSCChangeRate:
	push rbp
	mov rbp, rsp
	push r15
	push r13
	
	mov r15, rdi

	call rdtsc
	mov r13, rax

	mov r11, [Arch_HPETAddress]

	; Start the HPET timer.
	mov rax, [r11+0x10]
	or rax, (1<<0)
	mov [r11+0x10], rax

	lea r11, [r11+0xf0]
	
.loop:
	cmp [r11], r15
	jnge .loop

.end:

	call rdtsc
	sub rax, r13

	pop r13
	pop r15
	leave
	ret