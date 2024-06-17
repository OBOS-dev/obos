OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(Arch_KernelEntryBootstrap)

PHDRS
{
    text        PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* Execute + Read */
    no_pti_text PT_LOAD    FLAGS((1 << 0) | (1 << 1) | (1 << 2)); /* Read + Write + Execute */
    rodata      PT_LOAD    FLAGS((1 << 2)) ;            /* Read only */
    data        PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ; /* Write + Read */
}

SECTIONS
{
    . = 0xffffffff80000000;
    KERNEL_BASE = .;

    .text : {
        *(.text .text.*)
    } :text

    . += CONSTANT(MAXPAGESIZE);

    Arch_no_pti_start = .;
    .no.pti.text : {
        *(.no.pti.text)
    } :no_pti_text
    Arch_no_pti_end = (. + CONSTANT(MAXPAGESIZE)-1) & (CONSTANT(MAXPAGESIZE)-1);
    
    . += CONSTANT(MAXPAGESIZE);

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . += CONSTANT(MAXPAGESIZE);

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
    }
    KERNEL_TOP = .;
}