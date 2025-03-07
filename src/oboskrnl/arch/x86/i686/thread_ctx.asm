; oboskrnl/arch/x86/i686/thread_ctx.asm

; Copyright (c) 2025 Omar Berrow

[bits 32]

section .text

global CoreS_SwitchToThreadContext:function default
global CoreS_CallFunctionOnStack:function default
global CoreS_SaveRegisterContextAndYield:function default

extern Arch_HasXSAVE
extern Core_Schedule

struc thread_ctx
align 8
.extended_ctx_ptr: resd 1
.cr3: resd 1
.frame: resd 18
.stackBase: resd 1
.stackSize: resd 1
endstruc

CoreS_SwitchToThreadContext:
    cli
    mov edi, [esp+4]

    mov ebx, [edi]
	cmp ebx, 0
	je .no_xstate

	mov eax, [Arch_HasXSAVE]
	cmp eax, 0
	je .no_xsave

	; Restore the extended state.
	xor ecx,ecx
	xgetbv
	xrstor [ebx]
	jmp .no_xstate

.no_xsave:
	fxrstor [ebx]

.no_xstate:
    add edi, 4

	; Restore CR3 (address space)
	mov eax, [edi]
	mov edx, cr3
	cmp eax, edx
	je .kernel_cr3

; We won't be able to use kernel memory anymore, so copy the context to the stack.
	mov esi, edi
	sub esp, 96 - 8 ; push that many bytes
	mov edi, esp
	mov ecx, 96 - 8 ; sizeof(thread_ctx) - 8 (the amount of bytes already accessed)
	rep movsb
	sub edi, 96 - 8

	mov cr3, eax
.kernel_cr3:
	add edi, 4

	; Skip interrupt_frame.cr3
	add edi, 4 

	mov ax, [edi]
	mov ds, ax
	add edi, 4

	; Skip ignored2
	add edi, 4

	mov esp, edi

	popad

	add esp, 0xc

	iretd

extern Arch_GetCPUTempStack
CoreS_CallFunctionOnStack:
    push ebp
    mov ebp, esp

    mov ecx, [ebp+8]
    
    push ecx
    call Arch_GetCPUTempStack
    pop ecx
    
	lea esp, [eax+0x10000]

    push dword [ebp+12]
    call ecx

    mov ebp, esp
    pop ebp
    ret

CoreS_SaveRegisterContextAndYield:
	pushfd
	cli
	mov eax, [esp+8]
	
	mov [eax+thread_ctx.frame+0xc], ebp
	mov [eax+thread_ctx.frame+0x14], edi
	mov [eax+thread_ctx.frame+0x18], esi
	mov [eax+thread_ctx.frame+0x1c], ebx
	mov edx, [esp+4]
	mov [eax+thread_ctx.frame+0x38], edx ; eip
	lea edx, [esp+8]
	mov [eax+thread_ctx.frame+0x38], edx ; esp
	mov edx, [esp]
	mov [eax+thread_ctx.frame+0x40], edx ; eflags
	mov dword [eax+thread_ctx.frame+0x3c], 0x8 ; cs
	mov dword [eax+thread_ctx.frame+0x48], 0x10 ; ss
	mov dword [eax+thread_ctx.frame+0x4], 0x10 ; ds

	mov ebx, [Arch_HasXSAVE]
	cmp ebx, 0
	je .no_xsave

	xor ecx, ecx
	xgetbv

	mov ebx, [eax+thread_ctx.extended_ctx_ptr]
	xsave [ebx]
	jmp .call_scheduler

.no_xsave:
	mov ebx, [eax+thread_ctx.extended_ctx_ptr]
	fxsave [ebx]

.call_scheduler:
	popfd

	push dword Core_Schedule
	push dword 0
	call CoreS_CallFunctionOnStack
	ret
