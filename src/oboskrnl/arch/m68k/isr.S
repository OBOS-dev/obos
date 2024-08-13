/*
 * oboskrnl/arch/m68k/isr.S
 *
 * Copyright (c) 2024 Omar Berrow
*/

.global isr_stub
.global Arch_SimulateIRQ
.extern Arch_IrqHandlers

isr_stub:
    .cfi_startproc simple
    .cfi_signal_frame

    // Disable IRQs
    or #0x700, %sr 

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
    move.l (%d0,Arch_IRQHandlers), %a0
    
    move.l %sp, -(%sp)
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
// Simulates an incoming IRQ on the vector passed.
Arch_SimulateIRQ:
// d0: Contains the VBR.
// d1: Contains the vector.
// a0: Contains the vector handler.
    move.l (4,%sp), %d1
    movec.l %vbr, %d0
    mulu #4, %d1
    move.l %d1, %a0
    move.w %d1, %sp@-
    move.l #ret, %sp@-
    move.w %sr, %sp@-
    adda.l %d0, %a0
    move.l (%a0), %a0
    jmp (%a0)
ret:
    rts