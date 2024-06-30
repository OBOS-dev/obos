OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(Arch_KernelEntryBootstrap)

PHDRS
{
    text        PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* Execute + Read */
    rodata      PT_LOAD    FLAGS((1 << 2)) ;            /* Read only */
    data        PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ; /* Write + Read */
    
    no_mm_text   PT_LOAD    FLAGS((1 << 0) | (1 << 2)); /* Read + Execute */
    no_mm_data   PT_LOAD    FLAGS((1 << 1) | (1 << 2)); /* Read + Write */
    no_mm_rodata PT_LOAD    FLAGS((1 << 2));            /* Read only */
}

SECTIONS
{
    . = 0xffffffff80000000;
    KERNEL_BASE = .;

    .text : {
        *(.text .text.*)
    } :text

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

    . += CONSTANT(MAXPAGESIZE);

    MmS_MMExclusionRangeStart = .;
    /* Functions */
    .no.mm.text : {
        *(.no.mm.text)
    } :no_mm_text
    . += CONSTANT(MAXPAGESIZE);
    /* RW Data. */
    .no.mm.data : {
        *(.no.mm.data)
    } :no_mm_data
    . += CONSTANT(MAXPAGESIZE);
    .no.mm.data : {
        *(.no.mm.bss)
    } :no_mm_data
    /* RO Data */
    .no.mm.rodata : {
        *(.no.mm.rodata)
    } :no_mm_rodata
    . += CONSTANT(MAXPAGESIZE);
    MmS_MMExclusionRangeEnd = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
    }
    KERNEL_TOP = .;
}