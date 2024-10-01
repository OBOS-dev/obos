/*
 * oboskrnl/arch/m68k/asm_helpers.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

.cpu 68040

// void setSR(uint32_t to);
// uint32_t getSR();
.global setSR
.global getSR
.global pflush
setSR:    
    move.l (+4,%a7), %d0
    move.w %d0, %sr
    rts
getSR:
    move.w %sr, %d0
    rts
pflush:
    move.l (4, %sp), %a0
    pflush %a0
    rts