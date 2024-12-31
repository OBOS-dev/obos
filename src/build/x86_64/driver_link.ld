OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(OBOS_DriverEntry)

PHDRS
{
    headers     PT_PHDR PHDRS;
    text        PT_LOAD FILEHDR PHDRS FLAGS((1 << 0) | (1 << 2)) ; /* Execute + Read */
    rodata      PT_LOAD FLAGS((1 << 2))            ; /* Read only */
    data        PT_LOAD FLAGS((1 << 1) | (1 << 2)) ; /* Write + Read */
    
/* 0x00010000=PF_OBOS_PAGEABLE */
    pageable_text   PT_LOAD FLAGS((1 << 0) | (1 << 2) | 0x00010000); /* Read + Execute + Pageable */
    pageable_data   PT_LOAD FLAGS((1 << 1) | (1 << 2) | 0x00010000); /* Read + Write + Pageable  */
    pageable_rodata PT_LOAD FLAGS((1 << 2) | 0x00010000);            /* Read only + Pageable */

    dynamic PT_DYNAMIC;
}


SECTIONS
{
    Drv_Base = .;
    . = 0x0000000000000000 + SIZEOF_HEADERS;

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
        *(.driverheader)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));

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
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    /* RO Data */
    .pageable.rodata : {
        *(.pageable.rodata)
    } :pageable_rodata
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
    }
    
    Drv_Top = .;
    
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .dynamic : {
        *(.dynamic)
    } :data :dynamic

    Drv_Top = .;
}
