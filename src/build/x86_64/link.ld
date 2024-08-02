OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(Arch_KernelEntryBootstrap)

PHDRS
{
    text        PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* Execute + Read */
    rodata      PT_LOAD    FLAGS((1 << 2)) ;            /* Read only */
    data        PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ; /* Write + Read */
    
    pageable_text   PT_LOAD    FLAGS((1 << 0) | (1 << 2)); /* Read + Execute */
    pageable_data   PT_LOAD    FLAGS((1 << 1) | (1 << 2)); /* Read + Write */
    pageable_rodata PT_LOAD    FLAGS((1 << 2));            /* Read only */
}

SECTIONS
{
    . = 0xffffffff80000000;
    KERNEL_BASE = .;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    MmS_MMPageableRangeStart = .;
    /* Functions */
    .pageable.text : {
        *(.pageable.text)
    } :pageable_text
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    /* RW Data. */
    .pageable.data : {
        *(.pageable.data)
    } :pageable_data
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .pageable.bss : {
        *(.pageable.bss)
    } :pageable_data
    /* RO Data */
    .pageable.rodata : {
        *(.pageable.rodata)
    } :pageable_rodata
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    MmS_MMPageableRangeEnd = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
    }
    KERNEL_TOP = .;
}