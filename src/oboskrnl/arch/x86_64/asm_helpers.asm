; oboskrnl/arch/x86_64/asm_helpers.asm
;
; Copyright (c) 2024 Omar Berrow

[BITS 64]

global _ZN4obos6getCR0Ev
global _ZN4obos6getCR2Ev
global _ZN4obos6getCR3Ev
global _ZN4obos6getCR4Ev
global _ZN4obos6getCR8Ev
global _ZN4obos5pauseEv
global _ZN4obos7getEFEREv
global _ZN4obos5rdmsrEj
global _ZN4obos5wrmsrEjm
global _ZN4obos4outbEth
global _ZN4obos4outwEtt
global _ZN4obos4outdEtj
global _ZN4obos3inbEt
global _ZN4obos3inwEt
global _ZN4obos3indEt
global _ZN4obos9__cpuid__EmmPjS0_S0_S0_
global _ZN4obos6invlpgEm
global _ZN4obos6wbinvdEv

_ZN4obos6getCR0Ev:
	push rbp
	mov rbp, rsp

	mov rax, cr0

	leave
	ret
_ZN4obos6getCR2Ev:
	push rbp
	mov rbp, rsp	

	mov rax, cr2
	
	leave
	ret
_ZN4obos6getCR3Ev:
	push rbp
	mov rbp, rsp	

	mov rax, cr3
	
	leave
	ret
_ZN4obos6getCR4Ev:
	push rbp
	mov rbp, rsp

	mov rax, cr4
	
	leave
	ret
_ZN4obos6getCR8Ev:
	push rbp
	mov rbp, rsp

	mov rax, cr8

	leave
	ret
_ZN4obos7getEFEREv:
	push rbp
	mov rbp, rsp

	mov ecx, 0xc0000080 ; EFER
	rdmsr
	shl rdx, 32
	or rax, rdx
	
	leave
	ret
_ZN4obos5rdmsrEj:
	push rbp
	mov rbp, rsp

	mov ecx, edi
	rdmsr
	shl rdx, 32
	or rax, rdx

	leave
	ret
_ZN4obos5wrmsrEjm:
	push rbp
	mov rbp, rsp

	mov ecx, edi
	mov eax, esi
	mov rdx, rsi
	shr rdx, 32
	wrmsr
	
	leave
	ret
_ZN4obos5pauseEv:
	pause
	ret
_ZN4obos4outbEth:
	mov dx, di
	mov al, sil
	out dx, al
	ret
_ZN4obos4outwEtt:
	mov dx, di
	mov ax, si
	out dx, ax
	ret
_ZN4obos4outdEtj:
	mov dx, di
	mov eax, esi
	out dx, eax
	ret
_ZN4obos3inbEt:
	mov eax, 0
	mov dx, di
	in al, dx
	ret
_ZN4obos3inwEt:
	mov eax, 0
	mov dx, di
	in ax, dx
	ret
_ZN4obos3indEt:
	mov dx, di
	in eax, dx
	ret
_ZN4obos9__cpuid__EmmPjS0_S0_S0_:
	push rbp
	mov rbp, rsp
	sub rsp, 16
	push rbx

	mov [rbp-0x8], rdx
	mov [rbp-0x10], rcx

	mov eax, edi
	mov ecx, esi
	cpuid

	cmp qword [rbp-0x8],0
	jz .no_rdx
	mov r11, [rbp-0x8]
	mov dword [r11], eax
.no_rdx:
	cmp qword [rbp-0x10],0
	jz .no_rcx
	mov r11, [rbp-0x10]
	mov dword [r11], ebx
.no_rcx:
	cmp r8,0
	jz .no_r8
	mov dword [r8], ecx
.no_r8:
	cmp r9,0
	jz .no_r9
	mov dword [r9], edx
.no_r9:
	pop rbx
	leave 
	ret

_ZN4obos6wbinvdEv:
	wbinvd
	ret
_ZN4obos6invlpgEm:
	invlpg [rdi]
	ret