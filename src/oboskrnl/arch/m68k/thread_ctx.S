/*
 * oboskrnl/arch/m68k/thread_ctx.S
 *
 * Copyright (c) 2024 Omar Berrow
*/

.section .bss
// top of stack
.skip 0x10000
Arch_TempStack:
// bottom of stack
.section .text

.global CoreS_SwitchToThreadContext
.global CoreS_SaveRegisterContextAndYield
.global CoreS_CallFunctionOnStack
.extern Core_GetIRQLVar
.extern CoreS_SetIRQL

CoreS_SwitchToThreadContext:
    or #0x2700, %sr // Disable all IRQs

    jsr Core_GetIRQLVar
    sub.l %d2, %d2
    move.b (%d0), %d2
    move.l (+4,%sp), %a0
    move.b (+0x4e,%a0), (%d0)

    move.l (+4,%sp), %a0
    move.l %d2, %sp@-
    sub.l %d1, %d1
    move.b (+0x4e,%a0), %d1
    move.l %d1, %sp@-
    jsr CoreS_SetIRQL
    add #8, %sp

    move.l (+4,%sp), %a0

    move.l %a0@+, %d1
    movec %d1, %urp

    move.l %a0@+, %a1
    move.l %a1, %sp
    
    movem.l %a0@+, %d0-%d7/%a0-%a6
    
    add #2, %a0

    move.w %a0@+, %sr
    move.l %a0@+, %d1
    move.l %d1, %sp@-
    rts
CoreS_SwitchToThreadContextEnd:
.extern Core_Schedule
CoreS_SaveRegisterContextAndYield:
    move.l %sp@+, %d1 // save the return address into d1
    move.l %d1, %sp@-
    move.l (+4,%sp), %a0
    // Save URP
    movec %urp, %d0
    move.l %d0, %a0@+
    // Save SP
    move.l %sp, %a1
	move.l %a1, %a0@+
    // movem.l %d0-%d7/%a0-%a6, %a0@+
    // Save GPRs
    move.l %d0, %a0@+
    move.l %d1, %a0@+
    move.l %d2, %a0@+
    move.l %d3, %a0@+
    move.l %d4, %a0@+
    move.l %d5, %a0@+
    move.l %d6, %a0@+
    move.l %d7, %a0@+
    move.l %a0, %a0@+
    move.l %a1, %a0@+
    move.l %a2, %a0@+
    move.l %a3, %a0@+
    move.l %a4, %a0@+
    move.l %a5, %a0@+
    move.l %a6, %a0@+
    // Padding
    move.w #0, %a0@+
    // Save SR
    move.w %sr, %a0@+
    // Save PC
    move.l %d1, %a0@+
    jsr Core_Schedule
// should be noreturn, but return just in case
    rts
CoreS_CallFunctionOnStack:
// uintptr_t(*func)(uintptr_t), uintptr_t userdata
    move.l %d2, %sp@-
    move.l %sp, %d2 // save sp into d2
    move.l #Arch_TempStack, %sp
    
    move.l (8, %d2), %a0
    move.l (12, %d2), %d0
    move.l %d0, %sp@-
    jsr (%a0)

    move.l %d2, %sp
    add #4, %sp
    rts