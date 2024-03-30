; oboskrnl/arch/x86_64/lapic_timer_calibration.asm
;
; Copyright (c) 2024 Omar Berrow

[BITS 64]

global FindCounter
extern _ZN4obos18g_localAPICAddressE
extern _ZN4obos4arch13g_hpetAddressE
extern calibrateHPET

; uint64_t FindCounter(uint64_t hz);
; (input)  rdi: The expected frequency of the LAPIC count.
; (output) rax: The timer count for the LAPIC, assuming the divisor is LAPIC_TIMER_DIVISIOR_ONE (0b1101)
FindCounter:
	push rbp
	mov rbp, rsp
	push r15
	push r13
	
	call calibrateHPET
	mov r15, rax
	mov r13, [_ZN4obos18g_localAPICAddressE]
	mov r11, [_ZN4obos4arch13g_hpetAddressE]

	mov r13, [_ZN4obos18g_localAPICAddressE]
	mov dword [r13+0x3E0], 0xD ; DIVISOR_ONE
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