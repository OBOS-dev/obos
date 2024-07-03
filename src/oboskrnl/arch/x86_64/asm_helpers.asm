; oboskrnl/arch/x86_64/asm_helpers.asm
;
; Copyright (c) 2024 Omar Berrow

[BITS 64]

global getCR0
global getCR2
global getCR3
global getCR4
global getCR4
global getCR8
global getDR6
global pause
global getEFER
global rdmsr
global wrmsr
global outb
global outw
global outd
global inb
global inw
global ind
global __cpuid__
global invlpg
global wbinvd
global xsave
global cli
global sti
global hlt
global MmS_GetCurrentPageTable

section .no.mm.text

getCR0:
	push rbp
	mov rbp, rsp

	mov rax, cr0

	leave
	ret
getCR2:
	push rbp
	mov rbp, rsp	

	mov rax, cr2
	
	leave
	ret
getCR3:
	push rbp
	mov rbp, rsp	

	mov rax, cr3
	
	leave
	ret
getCR4:
	push rbp
	mov rbp, rsp

	mov rax, cr4
	
	leave
	ret
getCR8:
	push rbp
	mov rbp, rsp

	mov rax, cr8

	leave
	ret
getDR6:
	push rbp
	mov rbp, rsp

	mov rax, dr6

	leave
	ret
getEFER:
	push rbp
	mov rbp, rsp

	mov ecx, 0xc0000080 ; EFER
	rdmsr
	shl rdx, 32
	or rax, rdx
	
	leave
	ret
rdmsr:
	push rbp
	mov rbp, rsp

	mov ecx, edi
	rdmsr
	shl rdx, 32
	or rax, rdx

	leave
	ret
wrmsr:
	push rbp
	mov rbp, rsp

	mov ecx, edi
	mov eax, esi
	mov rdx, rsi
	shr rdx, 32
	wrmsr
	
	leave
	ret
pause:
	pause
	ret
outb:
	mov dx, di
	mov al, sil
	out dx, al
	ret
outw:
	mov dx, di
	mov ax, si
	out dx, ax
	ret
outd:
	mov dx, di
	mov eax, esi
	out dx, eax
	ret
inb:
	mov eax, 0
	mov dx, di
	in al, dx
	ret
inw:
	mov eax, 0
	mov dx, di
	in ax, dx
	ret
ind:
	mov dx, di
	in eax, dx
	ret
__cpuid__:
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

wbinvd:
	wbinvd
	ret
invlpg:
	invlpg [rdi]
	ret
xsave:
	xor rcx,rcx
	xgetbv
	xsave [rdi]
	ret
global CoreS_GetCPULocalPtr
extern Arch_SMPInitialized
CoreS_GetCPULocalPtr:
	push rbp
	mov rbp, rsp

	mov ecx, 0xC0000101
	rdmsr
	shl rdx, 32
	or rax, rdx

	cmp byte [Arch_SMPInitialized], 1
	jne .done
	cmp rax, 0
	jne .done
	mov rdi, 0xA50E7707 ; ASMERROR
	mov dword [rdi], 0xDEADBEEF

.done:

	leave 
	ret
cli:
	cli
	ret
sti:
	sti
	ret
hlt:
	hlt
	ret
MmS_GetCurrentPageTable:
	jmp getCR3