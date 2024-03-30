; oboskrnl/arch/x86_64/thr_context_save.asm

; Copyright (c) 2024 Omar Berrow

[BITS 64]

; Exported functions
global _ZN4obos4arch18SwitchToThrContextEPNS0_17ThreadContextInfoE
global _ZN4obos4arch11YieldThreadEPNS_9scheduler6ThreadE
global idleTask
; Exported variables

; External functions
extern _ZN4obos4arch17SaveThreadContextEPNS0_17ThreadContextInfoEPNS_15interrupt_frameE
extern _ZN4obos9scheduler8scheduleEv
extern _ZN4obos10getIRQLVarEv
extern GetContextInfoOffset
extern GetLastPreemptTimeOffset
extern RaiseIRQLForScheduler

; External variables
extern _ZN4obos4arch17ThreadContextInfo10xsave_sizeE
extern _ZN4obos9scheduler7g_ticksE

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
push 0 ; unused
push rbp
%endmacro

_ZN4obos4arch18SwitchToThrContextEPNS0_17ThreadContextInfoE:
	mov rax, [rdi]
	cmp rax, 0
	je .no_xstate
	; Restore the extended state.
	xrstor [rax]
.no_xstate:
	cli ; Disable interrupts to avoid getting an interrupt upon setting the IRQL.
	add rdi, 8
	
	; Restore the previous page map.
	mov rax, [rdi]
	mov cr3, rax
	add rdi, 8
	; Restore the IRQL.
	push rdi
	mov rdi, [rdi]
	push rdi
	call _ZN4obos10getIRQLVarEv
	pop rdi
	mov [rax], rdi
	mov cr8, rdi
	pop rdi
	add rdi, 8
	; Restore GS/FS base.
	mov rcx, 0xC0000101 ; GS_BASE
	mov eax, [rdi]
	mov edx, [rdi+4]
	wrmsr
	add rdi, 8
	dec rcx
	mov eax, [rdi]
	mov edx, [rdi+4]
	wrmsr
	add rdi, 8
	; Restore all GPRs.
	mov rsp, rdi
	add rsp, 8 ; Skip the saved DS
	popaq
	; Restore RSP, RFLAGS, CS, SS, and RIP.
	add rsp, 0x18
	iretq
_ZN4obos4arch11YieldThreadEPNS_9scheduler6ThreadE:
; Reserve the stack space.
	push 0
	push 0
	push 0
	push 0
	push 0
	push 0
	push 0
	push 0
; Instead of reserving it, just write it directly.
	pushaq
	; Just not here though
	mov rax, ds
	push rax
	lea rax, [rsp+0xd0] ; The rsp before the call
	mov [rsp+0xc0], rax ; The "interrupt" frame's saved RSP.
	mov rax, [rsp+0xd0] ; The real saved RIP.
	mov [rsp+0xa8], rax ; The "interrupt" frame's saved RIP.
	mov rax, cs ; Current CS
	mov [rsp+0xb0], rax ; The "interrupt" frame's saved CS.
	pushfq
	pop rax ; RFLAGS at the time of the call. This is guaranteed to be the same, as the none of the instructions used up to now modify RFLAGS.
	mov [rsp+0xb8], rax ; The "interrupt" frame's saved RFLAGS.
	mov rax, ss ; The current SS
	mov [rsp+0xc8], rax ; The "interrupt" frame's saved SS.
	mov rax, ds
	mov [rsp+0], rax
	
	push rdi
	call GetLastPreemptTimeOffset
	pop rdi
	mov rdx, [_ZN4obos9scheduler7g_ticksE]
	mov qword [rdi+rax], rdx
	
	; rdi = &thread->context
	push rdi
	call GetContextInfoOffset
	pop rdi
	add rdi, rax
	mov rsi, rsp
	push rdi
	call _ZN4obos4arch17SaveThreadContextEPNS0_17ThreadContextInfoEPNS_15interrupt_frameE
	call RaiseIRQLForScheduler
	pop rdi
	; We've (finally) saved the thread context, we can yield.
	call _ZN4obos9scheduler8scheduleEv
; If we got rescheduled, we would've already returned to the caller, as while setting up the interrupt frame, the RIP was the return address pushed by call.
; If not, hang
	jmp $
idleTask:
	hlt
	jmp idleTask