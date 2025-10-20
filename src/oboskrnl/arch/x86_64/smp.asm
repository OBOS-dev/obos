; oboskrnl/arch/x86_64/smp.asm
;
; Copyright (c) 2024 Omar Berrow

[BITS 64]
[DEFAULT ABS]

global Arch_SMPTrampolineStart:data hidden
global Arch_SMPTrampolineEnd:data hidden
global Arch_SMPTrampolineCR3:data hidden
global Arch_SMPTrampolineRSP:data hidden
global Arch_SMPTrampolineCPULocalPtr:data hidden

section .pageable.data

extern Arch_APEntry
global Arch_SMPTrampolineWakeLocation
Arch_SMPTrampolineWakeLocation: dq Arch_APEntry

Arch_SMPTrampolineStart:
bits 16
real_mode:
	jmp 0x0:start_addr
align 16
gdt:
trampoline_base: equ 0x1000
gdt_addr: equ $-Arch_SMPTrampolineStart+trampoline_base
	dq 0
	dq 0x00af9b000000ffff ; code segment, 64-bit
	dq 0x00cf93000000ffff ; data segment, 64-bit
gdtr_addr: equ $-Arch_SMPTrampolineStart+trampoline_base
gdtr:
	dw gdtr_addr-gdt_addr
	dd gdt_addr
cr3_loc: equ $-Arch_SMPTrampolineStart+trampoline_base
Arch_SMPTrampolineCR3:
	dq 0
rsp_loc: equ $-Arch_SMPTrampolineStart+trampoline_base
Arch_SMPTrampolineRSP:
	dq 0
cpu_local_loc: equ $-Arch_SMPTrampolineStart+trampoline_base
Arch_SMPTrampolineCPULocalPtr:
	dq 0
align 1
start:
start_addr: equ $-Arch_SMPTrampolineStart+trampoline_base
; Load GDT.
	lgdt [gdtr_addr]

; Enter long mode.
	xor ax,ax
	mov ds, ax
	mov ss, ax
	mov es, ax
	mov fs, ax
	mov gs, ax

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
    or eax, (1 << 8)|(1 << 10)|(1 << 0)
    xor edx,edx
	wrmsr

	mov eax, 0x80010001
	mov cr0, eax

	mov eax, [gdt_addr+0x8+4]
	test eax, 0x00af9b00
	jz .down1
	; weird...
	; TODO: Find the real cause of this bug.
	mov dword [gdt_addr+0x8], 0x0000ffff
	mov dword [gdt_addr+0x8+4], 0x00af9b00

.down1:

	mov eax, [gdt_addr+0x10+4]
	test eax, 0x00cf9300
	jz .down2
	; weird...
	; TODO: Find the real cause of this bug.
	mov dword [gdt_addr+0x10], 0x0000ffff
	mov dword [gdt_addr+0x10+4], 0x00cf9300

.down2:

	mov word [gdtr_addr+0], gdt_addr-gdtr_addr
	mov dword [gdtr_addr+0x2], gdt_addr
	lgdt [gdtr_addr]

; Reload segment registers.
	jmp 0x8:reload_cs_addr
.reload_cs:
reload_cs_addr: equ $-Arch_SMPTrampolineStart+trampoline_base
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
	mov rsp, [rsp_loc]

; Call AP initialization code.
	mov rdi, [cpu_local_loc]
    mov rax, [Arch_SMPTrampolineWakeLocation]
    call rax
Arch_SMPTrampolineEnd:
;global Arch_ACPIWakeTrampoline
;global Arch_ACPIWakeTrampoline_data
;global Arch_ACPIWakeTrampoline_end
;Arch_ACPIWakeTrampoline:
;bits 16
;	xor ax,ax
;	mov es, ax
;	mov ds, ax

;	mov si, Arch_ACPIWakeTrampoline_data-Arch_ACPIWakeTrampoline+0x1000
;	xor di,di

;	mov cx, Arch_SMPTrampolineEnd-Arch_SMPTrampolineStart
;	rep movsb

;	jmp 0x00:0x0000PolarityActiveHigh
;Arch_ACPIWakeTrampoline_data:
;times Arch_SMPTrampolineEnd-Arch_SMPTrampolineStart db 0
;Arch_ACPIWakeTrampoline_end:
;bits 64
section .text
; All of these CPUID bits are in CPUID.07H.0H:EBX
%define CPUID_FSGSBASE (1<<0)
%define CPUID_SMEP (1<<7)
%define CPUID_SMAP (1<<20)
%define CR4_FSGSBASE (1<<16)
%define CR4_SMEP (1<<20)
%define CR4_SMAP (1<<21)
global Arch_InitializeMiscFeatures
Arch_InitializeMiscFeatures:
	push rbp
	mov rbp, rsp
	sub rsp, 8
	mov [rbp-8], rbx

	mov eax, 7
	xor ecx, ecx
	cpuid

	mov rax, cr4

; NOTE: We don't use this in the kernel, but userspace might like it.
	test ebx, CPUID_FSGSBASE
	jz .next1
	or rax, CR4_FSGSBASE
.next1:
	test ebx, CPUID_SMEP
	jz .next2
	or rax, CR4_SMEP
.next2:
	test ebx, CPUID_SMAP
	jz .next3
	or rax, CR4_SMAP
.next3:

	mov cr4, rax

	mov rbx, [rbp-8]
	leave
	ret
global Arch_APYield:function hidden
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
