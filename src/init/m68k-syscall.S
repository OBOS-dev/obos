// This helper library allows one to use syscalln(...) in a user-program, since obos'
// sysdep marks it as hidden

.global syscall

syscall:
    move.l #0, %a1
    rts
