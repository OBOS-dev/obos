/*
 * oboskrnl/arch/m68k/asm_helpers.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

// void setSR(uint32_t to);
// uint32_t getSR();
.global setSR
.global getSR

setSR:
    link %a6,#-0
    
    move.l %a6,-(%sp)
    move.l %d0,(-8,%a6)
    move.w %d0, %sr
    
    unlk %a6
    rts
getSR:
    link %a6,#-0

    move.w %sr, %d0

    unlk %a6
    rts