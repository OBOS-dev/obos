// This helper library allows one to use syscalln(...) in a user-program, since obos'
// sysdep marks it as hidden

.intel_syntax noprefix

.global syscall

syscall:
    push rbp
    mov rbp, rsp

    mov eax, edi
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx

    syscall

    leave
    ret

.att_syntax prefix