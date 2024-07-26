; oboskrnl/arch/x86_64/smp.asm
;
; Copyright (c) 2024 Omar Berrow

[BITS 64]

global Arch_SMPTrampolineStart
global Arch_SMPTrampolineEnd
global Arch_SMPTrampolineCR3
global Arch_SMPTrampolineRSP
global Arch_SMPTrampolineCPULocalPtr

section .pageable.data

Arch_SMPTrampolineStart:
bits 16
real_mode:
	jmp start
align 16
gdt_addr: equ $-Arch_SMPTrampolineStart
	dq 0
	dq 0x00af9b000000ffff ; code segment, 64-bit
	dq 0x00cf93000000ffff ; data segment, 64-bit
gdtr_addr: equ $-Arch_SMPTrampolineStart
gdtr:
	dw gdtr_addr-gdt_addr
	dd gdt_addr
cr3_loc: equ $-Arch_SMPTrampolineStart
Arch_SMPTrampolineCR3:
	dq 0
rsp_loc: equ $-Arch_SMPTrampolineStart
Arch_SMPTrampolineRSP:
	dq 0
cpu_local_loc: equ $-Arch_SMPTrampolineStart
Arch_SMPTrampolineCPULocalPtr:
	dq 0
align 1
start:
; Load GDT.
	lgdt [gdtr_addr]

; Enter long mode.
	mov ax, 0
	mov ds, ax

	mov eax, cr4
	or eax, (1<<5)
	mov cr4, eax

	mov eax, [ds:cr3_loc]
	mov cr3, eax

	mov eax, 0x80000001
	xor ecx,ecx
	cpuid
	xor eax, eax
	test edx, (1<<20)
	mov esi, (1<<11)
	cmovnz eax, esi ; If bit 20 is set
	mov ecx, 0xC0000080
    or eax, (1 << 8)|(1<<10)
    xor edx,edx
	wrmsr

	mov eax, 0x80010001
	mov cr0, eax

; Reload segment registers.
	jmp 0x8:reload_cs_addr
.reload_cs:
reload_cs_addr: equ $-Arch_SMPTrampolineStart
	mov ax, 0x10
	mov ss, ax
	mov ds, ax
	mov gs, ax
	mov fs, ax
	mov es, ax
[BITS 64]

; Reload cr3.
	mov rax, cr3
	mov cr3, rax

; Load RSP.
	mov rsp, [Arch_SMPTrampolineRSP-Arch_SMPTrampolineStart]

extern Arch_APEntry
; Call AP initialization code.
	mov rdi, [Arch_SMPTrampolineCPULocalPtr-Arch_SMPTrampolineStart]
	mov rax, Arch_APEntry
	call rax
Arch_SMPTrampolineEnd:
global Arch_APYield
extern Core_Yield
extern OBOS_BasicMMFreePages
extern OBOS_Panic
section .rodata
panic_str: db "Arch_APYield: Core_Yield returned.", 0xa, 0x0
section .text
Arch_APYield:
	lea rsp, [rsi+0x10000]
	push rdi
	push rsi
	mov rsi, 0x4000
	call OBOS_BasicMMFreePages
	pop rsi
	pop rdi
	call Core_Yield
	mov rdi, 1 ; OBOS_PANIC_REASON_FATAL_ERROR
	mov rsi, panic_str
	xor rax, rax
	call OBOS_Panic
	jmp $