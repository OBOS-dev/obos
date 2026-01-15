; oboskrnl/arch/x86_64/asm_helpers.asm
;
; Copyright (c) 2024-2026 Omar Berrow

[BITS 64]
[DEFAULT ABS]

global getCR0:function default
global getCR2:function default
global getCR3:function default
global getCR4:function default
global getCR4:function default
global getCR8:function default
global getDR6:function default
global pause:function default
global getEFER:function default
global rdmsr:function default
global wrmsr:function default
global outb:function default
global outw:function default
global outd:function default
global inb:function default
global inw:function default
global ind:function default
global __cpuid__:function default
global invlpg:function default
global wbinvd:function default
global xsave:function default
global cli:function default
global sti:function default
global hlt:function default
global MmS_GetCurrentPageTable: function default

section .text

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
global CoreS_GetCPULocalPtr:function default
extern Arch_cpu_local_curr_offset
extern Arch_SMPInitialized
CoreS_GetCPULocalPtr:
	push rbp
	mov rbp, rsp

	mov rax, [Arch_cpu_local_curr_offset]
	mov rax, [gs:rax]

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
global rdtsc
rdtsc:
	xor eax,eax
	xor edx,edx
	rdtsc
	shl rdx, 32
	or rax, rdx
	ret
MmS_GetCurrentPageTable:
	jmp getCR3

extern Arch_cpu_local_currentIrql_offset
global Core_RaiseIrqlNoThread: function weak
Core_RaiseIrqlNoThread:
	mov rax, [Arch_cpu_local_currentIrql_offset]
	xchg [gs:rax], rdi
	mov rax, rdi
	ret