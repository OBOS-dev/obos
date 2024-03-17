; oboskrnl/arch/x86_64/smp_trampoline.asm

; Copyright (c) 2024 Omar Berrow

[DEFAULT REL]

section .data

[BITS 16]
global smp_trampoline_start
global smp_trampoline_end
global smp_trampoline_cr3_loc
global smp_trampoline_cpu_local_ptr
global smp_trampoline_pat

extern _ZN4obos4arch9ProcStartEPNS_9scheduler9cpu_localE


smp_trampoline_start:
	jmp .real_mode
align 8
.gdt:
	dq 0
; 64-bit Code segment.
	dq 0x00209A0000000000
; 64-bit Data segment.
	dq 0x0000920000000000
.gdt_end:
.gdtr: equ 0x8+.gdt_end-.gdt
.cpu_local_ptr: equ smp_trampoline_cpu_local_ptr-smp_trampoline_start
.cr3_loc: equ smp_trampoline_cr3_loc-smp_trampoline_start
.reload_cs_addr: equ .reload_cs-smp_trampoline_start
.pat: equ smp_trampoline_pat-smp_trampoline_start
dw .gdt_end-.gdt-1
dq 0x8
align 1
.real_mode:
	; Use a method to get from real mode->long mode.
	lgdt [.gdtr]

	mov ax, 0
	mov ds, ax

	mov eax, cr4
	or eax, (1<<5)
	mov cr4, eax

	mov eax, [ds:.cr3_loc]
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

; We're in long mode, we just need to a couple things before jumping to the bootstrap code.
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov fs, ax
	mov gs, ax

	jmp 0x8:.reload_cs_addr
.reload_cs:
[BITS 64]
; Set the PAT properly.
	mov eax, dword [abs .pat]
	mov edx, dword [abs .pat+4]
	mov ecx, 0x277 ; IA32_PAT
	wrmsr
; Reload cr3 to reload any caching info in the TLB.
	mov rax, cr3
	mov cr3, rax

; Load the startup stack.
	mov rax, [abs .cpu_local_ptr]
	mov rdx, [rax]
	add rdx, [rax+0x10]
	mov rsp, rdx

; Jump to the bootstrap function.
	mov rdi, [abs .cpu_local_ptr]
	mov rax, _ZN4obos4arch9ProcStartEPNS_9scheduler9cpu_localE
	jmp rax

smp_trampoline_cr3_loc:
	dq 0
smp_trampoline_cpu_local_ptr:
	dq 0
smp_trampoline_pat:
	dq 0
smp_trampoline_end:
section .text
global _ZN4obos9scheduler9GetCPUPtrEv
extern _ZN4obos5rdmsrEj
_ZN4obos9scheduler9GetCPUPtrEv:
	mov rdi, 0xc0000101
	jmp _ZN4obos5rdmsrEj
global reload_gdt
reload_gdt:
	push rbp
	mov rbp, rsp
	lgdt [rdi]
	
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov fs, ax
	mov gs, ax

	mov ax, 0x28
	ltr ax

	leave
	pop rax
	push 0x8
	push rax
	o64 retf