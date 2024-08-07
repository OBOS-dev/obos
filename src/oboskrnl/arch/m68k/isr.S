/*
 * oboskrnl/arch/m68k/isr.S
 *
 * Copyright (c) 2024 Omar Berrow
*/

.global isr_stub
.extern Arch_IrqHandlers

isr_stub:
    .cfi_startproc
    .cfi_signal_frame

// Align the stack
    move.w #0, -(%sp)
    .cfi_def_cfa %sp, 0
	.cfi_offset %pc, 4

// Push the GPRs d0-d7 and a0-a6.
// Functionally equivalent to a pushad on x86
    movem.l %d0-%d7/%a0-%a6, %sp@-
   .cfi_adjust_cfa_offset 60

// Push the user stack pointer
    move.l %usp, %a0
	move.l %a0, %sp@-
	.cfi_adjust_cfa_offset 4

// Push 'vector'
    move.l #0, %d0
    move.l #0, %d1
    move.w (0x48,%sp), %d0
    and #0xfff, %d0
    divu #4, %d0
    move.w %d0, %d1
    sub.l #64, %d1
    move.l %d1, -(%sp)
    move.l %d0, -(%sp)
	.cfi_adjust_cfa_offset 8

// Call the handler
    mulu #4, %d0
    move.l (%d0,Arch_IrqHandlers), %a0
    
    move.l %sp, %d0
    move.l %d0, -(%sp)
	.cfi_adjust_cfa_offset 4
    
    cmp #0, %a0
    beq .L1

    jsr (%a0)

.L1:
// Restore GPRs

    add #12, %sp
    .cfi_adjust_cfa_offset -12

    move.l %sp@+, %a0
    move.l %a0, %usp
    .cfi_adjust_cfa_offset -4

    movem.l %sp@+, %d0-%d7/%a0-%a6
    .cfi_adjust_cfa_offset -60
    
    add #2, %sp

    rte
    .cfi_endproc