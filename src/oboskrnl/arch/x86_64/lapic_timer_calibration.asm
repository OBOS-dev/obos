; oboskrnl/arch/x86_64/lapic_timer_calibration.asm
;
; Copyright (c) 2024 Omar Berrow

[BITS 64]

global FindCounter
extern _ZN4obos18g_localAPICAddressE
extern _ZN4obos4arch13g_hpetAddressE

; uint64_t FindCounter(uint64_t hz);
; (input)  rdi: The expected frequency of the LAPIC count.
; (output) rax: The timer count for the LAPIC, assuming the divisor is LAPIC_TIMER_DIVISIOR_ONE (0b1101)
FindCounter:
	push rbp
	mov rbp, rsp
	push r15
	push r14
	push r13
	
	mov r11, [_ZN4obos4arch13g_hpetAddressE]
	mov r13, [_ZN4obos18g_localAPICAddressE]
	
	; Calculate the HPET frequency, putting it into r14
	xor rdx, rdx
	mov rax, 1000000000000000
	div qword [r11+0x4]
	mov r14, rax
	
	; Stop the HPET timer.
	mov rax, [r11+0x10]
	and rax, ~(1<<0)
	mov [r11+0x10], rax
	
	; mainCounterValue is at r11+0xE8
	; timer0 (base) is at r11+0xF8
	; Calculate the comparator value
	mov rax, [r11+0xE8]
	push rax
	xor rdx, rdx
	mov rax, r14
	div rdi
	mov r10, rax
	pop rax
	add rax, r10
	mov [r11+0xF8+0x8], rax
	mov r15, rax
	
	; I don't know why I did this in the previous rewrite.
	mov rax, [r11+0xF8+0x0]
	mov r10, rax
	and r10, ~(1<<2)
	test rax, (1<<3)
	cmovnz rax, r10
	mov [r11+0xF8+0x0], rax

; r15 has the comparator value.
	
	; Start the HPET timer.
	mov rax, [r11+0x10]
	or rax, (1<<0)
	mov [r11+0x10], rax
	
	mov dword [r13+0x3E0], 0xD ; DIVISOR_ONE
	mov dword [r13+0x380], 0xffffffff
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
	pop r14
	pop r15
	leave
	ret