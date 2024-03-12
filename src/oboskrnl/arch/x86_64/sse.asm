; oboskrnl/arch/x86_64/sse.asm

; Copyright (c) 2024 Omar Berrow

[BITS 64]

[global enableSSE]
[global enableXSAVE]
[global enableAVX]
[global enableAVX512]

enableSSE:
	push rbp
	mov rbp, rsp

	mov rax, cr0
	and rax, ~(1<<2)
	or rax, (1<<2)
	mov cr0, rax
	mov rax, cr4
	or rax, (1<<9)|(1<<10)
	mov cr4, rax

	leave
	ret
enableXSAVE:
	push rbp
	mov rbp, rsp
	push rbx

	mov eax, 1
	xor ecx, ecx
	cpuid
	test ecx, (1<<26)
	jz .done
	; Set CR4.OSXSAVE
	mov rax, cr4
	or rax, (1<<18) 
	mov cr4, rax

.done:
	pop rbx
	leave
	ret
enableAVX:
	push rbp
	mov rbp, rsp
	push rbx

	mov eax, 1
	xor ecx, ecx
	cpuid
	test ecx, (1<<28)
	jz .done
	xor rcx,rcx
	xgetbv
	or eax, (1<<0)|(1<<1)|(1<<2)
	xsetbv
	
.done:
	pop rbx
	leave 
	ret
enableAVX512:
	push rbp
	mov rbp, rsp
	push rbx

	mov eax, 0xD
	xor ecx, ecx
	cpuid
	test eax, (1<<5)|(1<<6)|(1<<7)
	jz .done
	xor rcx,rcx
	xgetbv
	or eax, (1<<5)|(1<<6)|(1<<7)
	xsetbv
	
.done:
	pop rbx
	leave
	ret