/*
 * oboskrnl/elf/elf64.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

#define EV_NONE 0
#define EV_CURRENT 1

#define EI_NIDENT 16
#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
// Must be EV_CURRENT.
#define EI_VERSION 6
#define EI_PAD 7

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4
#define PF_OBOS_PAGEABLE 0x00010000 /* in PF_MASKOS */
#define PF_MASKOS 0x00FF0000

typedef uintptr_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uintptr_t Elf32_Off;
typedef uint32_t Elf32_Word;
typedef int32_t Elf32_Sword;

enum
{
	PT_NULL,
	PT_LOAD,
	PT_DYNAMIC,
	PT_INTERP,
	PT_NOTE,
	PT_SHLIB,
	PT_PHDR
};

enum
{
	ET_NONE,
	ET_REL,
	ET_EXEC,
	ET_DYN,
	ET_CORE,
	ET_LOPROC = 0xff00,
	ET_HIPROC = 0xffff,
};

enum
{
	SHT_NULL,
	SHT_PROGBITS,
	SHT_SYMTAB,
	SHT_STRTAB,
	SHT_RELA,
	SHT_HASH,
	SHT_DYNAMIC,
	SHT_NOTE,
	SHT_NOBITS,
	SHT_REL,
	SHT_SHLIB,
	SHT_DYNSYM,
	SHT_INIT_ARRAY,
	SHT_FINI_ARRAY,
	SHT_PREINIT_ARRAY,
	SHT_GROUP,
	SHT_SYMTAB_SHNDX,
};

enum
{
	STB_LOCAL,
	STB_GLOBAL,
	STB_WEAK,
	STB_LOOS,
	STB_HIOS,
};

enum
{
	STT_NOTYPE,
	STT_OBJECT,
	STT_FUNC,
	STT_SECTION,
	STT_FILE,
	STT_COMMON,
	STT_TLS,
	STT_LOOS,
	STT_HIOS,
	STT_LOPROC,
	STT_HIPROC,
};

enum
{
	STV_DEFAULT,
	STV_INTERNAL,
	STV_HIDDEN,
	STV_PROTECTED,
	STV_EXPORTED,
	STV_SINGLETON,
	STV_ELIMINATE,
};

enum
{
	EM_68K = 4,
#ifdef __m68k__
	EM_CURRENT = EM_68K,
#endif
};

#define ELF32_ST_BIND(info)          ((info) >> 4)
#define ELF32_ST_TYPE(info)          ((info) & 0xf)
#define ELF32_ST_INFO(bind, type)    (((bind)<<4)+((type)&0xf))
#define ELF_ST_BIND(info)       ELF32_ST_BIND(info)       
#define ELF_ST_TYPE(info)       ELF32_ST_TYPE(info)
#define ELF_ST_INFO(bind, type) ELF32_ST_INFO(bind, type)

typedef struct Elf32_Shdr
{
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off  sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
} Elf32_Shdr, Elf_Shdr;

typedef struct Elf32_Phdr
{
	Elf32_Word p_type;
	Elf32_Off p_offset;
	Elf32_Addr p_vaddr;
	Elf32_Addr p_paddr;
	Elf32_Word p_filesz;
	Elf32_Word p_memsz;
	Elf32_Word p_flags;
	Elf32_Word p_align;
} ELf32_Phdr, Elf_Phdr;

typedef struct Elf32_Sym
{
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half st_shndx;
} Elf32_Sym, Elf_Sym;

typedef struct {
	Elf32_Addr      r_offset;
	Elf32_Word     r_info;
} Elf32_Rel, Elf_Rel;

typedef struct {
	Elf32_Addr      r_offset;
	Elf32_Word     r_info;
	Elf32_Sword    r_addend;
} Elf32_Rela, Elf_Rela;

typedef struct
{
	unsigned char e_ident[EI_NIDENT];
	Elf32_Half e_type;
	Elf32_Half e_machine;
	Elf32_Word e_version;
	Elf32_Addr e_entry;
	Elf32_Off  e_phoff;
	Elf32_Off  e_shoff;
	Elf32_Word e_flags;
	Elf32_Half e_ehsize;
	Elf32_Half e_phentsize;
	Elf32_Half e_phnum;
	Elf32_Half e_shentsize;
	Elf32_Half e_shnum;
	Elf32_Half e_shstrndx;
} Elf32_Ehdr, Elf_Ehdr;

typedef struct Elf32_Dyn 
{
	Elf32_Sword d_tag;
	union
	{
		Elf32_Word d_val;
		Elf32_Addr d_ptr;
	} d_un;
} Elf32_Dyn, Elf_Dyn;

#ifdef __m68k__
// refer to sysv-m68k-abi-part3.pdf page 6 for info on relocations
enum
{
	R_68K_NONE,
	R_68K_32,
	R_68K_16,
	R_68K_8,
	R_68K_PC32,
	R_68K_PC16,
	R_68K_PC8,
	R_68K_GOT32,
	R_68K_GOT16,
	R_68K_GOT8,
	R_68K_GOT320,
	R_68K_GOT160,
	R_68K_GOT80,
	R_68K_PLT32,
	R_68K_PLT16,
	R_68K_PLT8,
	R_68K_PLT320,
	R_68K_PLT160,
	R_68K_PLT80,
	R_68K_COPY,
	R_68K_GLOB_DAT,
	R_68K_JUMP_SLOT,
	R_68K_RELATIVE,
};
#endif

// maybe I shouldn't copy this from elf64
enum
{
	DT_NULL,
	DT_NEEDED,
	DT_PLTRELSZ,
	DT_PLTGOT,
	DT_HASH,
	DT_STRTAB,
	DT_SYMTAB,
	DT_RELA,
	DT_RELASZ,
	DT_RELAENT,
	DT_STRSZ,
	DT_SYMENT,
	DT_INIT,
	DT_FINI,
	DT_SONAME,
	DT_RPATH,
	DT_SYMBOLIC,
	DT_REL,
	DT_RELSZ,
	DT_RELENT,
	DT_PLTREL,
	DT_DEBUG,
	DT_TEXTREL,
	DT_JMPREL,
	DT_BIND_NOW,
	DT_INIT_ARRAY,
	DT_FINI_ARRAY,
	DT_INIT_ARRAYSZ,
	DT_FINI_ARRAYSZ,
	DT_RUNPATH,
	DT_FLAGS,
	DT_ENCODING,
	DT_PREINIT_ARRAY,

	DT_RELACOUNT = 0x6ffffff9,
	DT_RELCOUNT,
};