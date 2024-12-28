; oboskrnl/arch/x86_64/thread_ctx.asm
;
; Copyright (c) 2024 Omar Berrow

[BITS 64]

; obos_status CoreS_SetupThreadContext(thread_ctx* ctx, uintptr_t entry, uintptr_t arg1, bool makeUserMode, void* stackBase, size_t stackSize);
; OBOS_NORETURN void CoreS_SwitchToThreadContext(const thread_ctx* ctx);
; void CoreS_SaveRegisterContextAndYield(thread_ctx* ctx);
; obos_status CoreS_FreeThreadContext(thread_ctx* ctx);
; uintptr_t CoreS_CallFunctionOnStack(uintptr_t(*func)(uintptr_t), uintptr_t userdata);
; 
; void CoreS_SetThreadIRQL(thread_ctx* ctx, irql newIRQL);
; irql CoreS_GetThreadIRQL(const thread_ctx* ctx);

global CoreS_SetupThreadContext:function default
global CoreS_SwitchToThreadContext:function hidden
global CoreS_SaveRegisterContextAndYield:function hidden
global CoreS_FreeThreadContext:function hidden
global CoreS_CallFunctionOnStack:function default
global CoreS_SetThreadIRQL:function hidden
global CoreS_GetThreadIRQL:function hidden
global CoreS_ThreadAlloca:function hidden

%macro popaq 0
pop rbp
add rsp, 8
pop r8
pop r9
pop r10
pop r11
pop r12
pop r13
pop r14
pop r15
pop rdi
pop rsi
add rsp, 8
pop rbx
pop rdx
pop rcx
pop rax
%endmacro

struc thread_ctx
align 8
.extended_ctx_ptr: resq 1
.irql: resq 1
.cr3: resq 1
.gs_base: resq 1
.fs_base: resq 1
.frame: resq 0x1b
.stackBase: resq 1
.stackSize: resq 1
endstruc

extern Core_GetIRQLVar
section .text
CoreS_SwitchToThreadContext:
	; Disable interrupts, getting an interrupt in the middle of execution of this function can be deadly.
	cli
	mov rbx, [rdi]
	cmp rbx, 0
	je .no_xstate
	; Restore the extended state.
	xor rcx,rcx
	xgetbv
	xrstor [rbx]
.no_xstate:
	add rdi, 8

; Restore IRQL.
	mov rax, [rdi]
	mov cr8, rax
	push rdi
	call Core_GetIRQLVar
	pop rdi
	mov rcx, [rdi]
	mov [rax], rcx
	add rdi, 8

	; Restore CR3 (address space)
	mov rax, [rdi]
	mov rdx, cr3
	cmp rax, rdx
	je .kernel_cr3

; We won't be able to use kernel memory anymore, so copy the context to the stack.
	mov rsi, rdi
	sub rsp, 272 - 8 ; push that many bytes
	mov rdi, rsp
	mov rcx, 272 - 8 ; sizeof(thread_ctx) - 8 (the amount of bytes already accessed)
	rep movsb
	sub rdi, 272 - 8

.kernel_cr3:
	mov cr3, rax
	add rdi, 8

	; Restore GS_BASE
	test qword [rdi+16+0xB8], 0x3
	je .restore_fs_base
	swapgs
	mov eax, [rdi]
	mov edx, [rdi+4]
	mov ecx, 0xC0000101
	wrmsr

.restore_fs_base:

	add rdi, 0x8
	; Restore FS_BASE
	mov eax, [rdi]
	mov edx, [rdi+4]
	mov ecx, 0xC0000100
	wrmsr
	add rdi, 8

	; Restore thread GPRs.
	mov rsp, rdi
	add rsp, 0x10 ; Skip the saved DS and CR3
	popaq
	add rsp, 0x18
	iretq
global CoreS_SwitchToThreadContextEnd: data hidden
CoreS_SwitchToThreadContextEnd:
section .pageable.text
CoreS_FreeThreadContext:
	push rbp
	mov rbp, rsp
	; TODO: Implement.

	leave
	ret
CoreS_SetupThreadContext:
	push rbp
	mov rbp, rsp

	;             rdi,             rsi,            rdx,               rcx,              r8,               r9
	; thread_ctx* ctx, uintptr_t entry, uintptr_t arg1, bool makeUserMode, void* stackBase, size_t stackSize

	cmp rdi, 0
	jnz .L1
	mov rax, 2 ; OBOS_STATUS_INVALID_ARGUMENT
	jmp .finish

.L1:
	; Setup the registers.
	mov [rdi+thread_ctx.frame+0xB0], rsi             ; ctx->frame.rip
	mov [rdi+thread_ctx.frame+0x60], rdx             ; ctx->frame.rdi
	mov qword [rdi+thread_ctx.frame+0xB8], 0x8       ; ctx->frame.cs
	mov qword [rdi+thread_ctx.frame+0xD0], 0x10      ; ctx->frame.ss
	mov qword [rdi+thread_ctx.frame+0xC0], 0x200202  ; ctx->frame.rflags
	lea rax, [r8+r9]
	mov qword [rdi+thread_ctx.frame+0xC8], rax       ; ctx->frame.rsp
	mov qword [rdi+thread_ctx.extended_ctx_ptr], 0   ; ctx->extended_ctx_ptr
	mov [rdi+thread_ctx.stackBase], r8
	mov [rdi+thread_ctx.stackSize], r9

	cmp rcx, 1
	jne .kmode

.userspace:
	mov qword [rdi+thread_ctx.frame+0xB8], 0x20|3 ; ctx->frame.cs=user (CPL3) data segment
	mov qword [rdi+thread_ctx.frame+0xD0], 0x18|3 ; ctx->frame.ss=user (CPL3) code segment

.kmode:

	; Setup the IRQL.
	mov byte [rdi+thread_ctx.irql], 0 ; Unmasked
	; Setup the page map.
	mov rax, cr3
	mov qword [rdi+thread_ctx.cr3], rax
	; Setup GS_BASE.
	;mov rcx, 0xC0000101 ; GS.Base
	;rdmsr
	;shl rdx, 32
	;or rax, rdx
	;mov qword [rdi+thread_ctx.gs_base], rax

	xor rax, rax ; OBOS_STATUS_SUCCESS
.finish:
	leave
	ret
global CoreS_SetThreadPageTable
CoreS_SetThreadPageTable:
	mov qword [rdi+thread_ctx.cr3], rsi
	ret
section .text
extern Arch_GetCPUTempStack
CoreS_CallFunctionOnStack:
	push rbp
	mov rbp, rsp

	push rdi
	push rsi
	call Arch_GetCPUTempStack
	pop rsi
	pop rdi
	lea rsp, [rax+0x20000]
	xchg rdi, rsi
	call rsi

	leave
	ret
extern Core_Schedule
extern Core_GetIRQLVar
CoreS_SaveRegisterContextAndYield:
	pushfq
	cli
	; Save the current GPRs.
	mov [rdi+thread_ctx.frame+0x10] , rbp
	mov qword [rdi+thread_ctx.frame+0x18], 0
	mov [rdi+thread_ctx.frame+0x40], r12
	mov [rdi+thread_ctx.frame+0x48], r13
	mov [rdi+thread_ctx.frame+0x50], r14
	mov [rdi+thread_ctx.frame+0x58], r15
	mov qword [rdi+thread_ctx.frame+0x70], 0
	mov [rdi+thread_ctx.frame+0x78], rbx
	mov rax, ds
	mov [rdi+thread_ctx.frame+0x08], rax ; ds
	mov rax, [rsp+8]
	mov [rdi+thread_ctx.frame+0xB0], rax ; return address
	mov qword [rdi+thread_ctx.frame+0xB8], 0x08 ; cs
	pop rax ; see pushfq at the beginning of the function
	push rax
	mov [rdi+thread_ctx.frame+0xC0], rax ; rflags
	lea rax, [rsp+0x10] ; skip the return address
	mov [rdi+thread_ctx.frame+0xC8], rax ; rsp
	mov qword [rdi+thread_ctx.frame+0xD0], 0x10 ; ss
	mov ecx, 0xC0000101
	rdmsr
	shl rdx, 32
	or rax, rdx
	mov [rdi+thread_ctx.gs_base], rax
	mov ecx, 0xC0000100
	rdmsr
	shl rdx, 32
	or rax, rdx
	mov [rdi+thread_ctx.fs_base], rax
	mov rax, cr3
	mov [rdi+thread_ctx.cr3], rax
	mov [rdi+thread_ctx.frame+0x00], rax ; cr3
	
	cmp qword [rdi+thread_ctx.extended_ctx_ptr], 0
	jz .call_scheduler
	mov rax, [rdi+thread_ctx.extended_ctx_ptr]
	xsave [rax]

.call_scheduler:
	popfq ; see the rflags saving code at the beginning of the function

	mov rdi, Core_Schedule
	xor rsi,rsi
	call CoreS_CallFunctionOnStack
	ret ; There is the chance that the scheduler returned because the threads quantum has not been finished yet.
; When the scheduler switches to the current thread context, the rip will be at the return address, as we set rip to the return address passed on the stack
CoreS_SetThreadIRQL:
	push rbp
	mov rbp, rsp

	mov [rdi+thread_ctx.irql], sil

	leave
	ret
CoreS_GetThreadIRQL:
	push rbp
	mov rbp, rsp

	mov rax, [rdi+thread_ctx.irql]

	leave
	ret
global CoreS_GetThreadStack
global CoreS_GetThreadStackSize
CoreS_GetThreadStackSize:
	push rbp
	mov rbp, rsp

	mov rax, [rdi+thread_ctx.stackSize]

	leave
	ret
CoreS_GetThreadStack:
	push rbp
	mov rbp, rsp

	mov rax, [rdi+thread_ctx.stackBase]

	leave
	ret

CoreS_ThreadAlloca:
	push rbp
	mov rbp, rsp

	cmp rdx, 0
	jz .status_nullptr1
	mov dword [rdx], 0 ; OBOS_STATUS_SUCCESS
.status_nullptr1:

	; ret = ctx->frame.rsp -= size
	mov rax, [rdi+thread_ctx.frame+0xc8]
	sub rax, rsi
	mov [rdi+thread_ctx.frame+0xc8], rax
	; if (ret < ctx->stackBase)
	cmp rax, [rdi+thread_ctx.stackBase]
	jae .done

	cmp rdx, 0
	jz .status_nullptr2
	; if (status) *status = OBOS_STATUS_NOT_ENOUGH_MEMORY
	mov dword [rdx], 6 ; OBOS_STATUS_NOT_ENOUGH_MEMORY
.status_nullptr2:
	; ret = nullptr
	xor rax,rax
.done:

	; return ret;
	leave
	ret

section .text
