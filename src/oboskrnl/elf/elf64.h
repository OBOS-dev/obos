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

typedef uintptr_t Elf64_Addr;
typedef uintptr_t Elf64_Off;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Qword;
typedef int64_t Elf64_SQword;
typedef uint16_t Elf64_Half;

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

#define ELF64_ST_BIND(info)          ((info) >> 4)
#define ELF64_ST_TYPE(info)          ((info) & 0xf)
#define ELF64_ST_INFO(bind, type)    (((bind)<<4)+((type)&0xf))
#define ELF_ST_BIND(info)       ELF64_ST_BIND(info)       
#define ELF_ST_TYPE(info)       ELF64_ST_TYPE(info)
#define ELF_ST_INFO(bind, type) ELF64_ST_INFO(bind, type)

enum
{
	EM_X86_64 = 62,
	
#ifdef __x86_64__
	EM_CURRENT = EM_X86_64,
#endif
};

typedef struct
{
	unsigned char e_ident[EI_NIDENT];
	Elf64_Half e_type;
	Elf64_Half e_machine;
	Elf64_Word e_version;
	Elf64_Addr e_entry;
	Elf64_Off  e_phoff;
	Elf64_Off  e_shoff;
	Elf64_Word e_flags;
	Elf64_Half e_ehsize;
	Elf64_Half e_phentsize;
	Elf64_Half e_phnum;
	Elf64_Half e_shentsize;
	Elf64_Half e_shnum;
	Elf64_Half e_shstrndx;
} Elf64_Ehdr, Elf_Ehdr;

typedef struct
{
	Elf64_Word p_type;
	Elf64_Word p_flags;
	Elf64_Off  p_offset;
	Elf64_Addr p_vaddr;
	Elf64_Addr p_paddr;
	Elf64_Qword p_filesz;
	Elf64_Qword p_memsz;
	Elf64_Qword p_align;
} Elf64_Phdr, Elf_Phdr;

typedef struct
{
	Elf64_Word 	sh_name;
	Elf64_Word 	sh_type;
	Elf64_Qword sh_flags;
	Elf64_Addr 	sh_addr;
	Elf64_Off 	sh_offset;
	Elf64_Qword sh_size;
	Elf64_Word 	sh_link;
	Elf64_Word 	sh_info;
	Elf64_Qword sh_addralign;
	Elf64_Qword sh_entsize;
} Elf64_Shdr, Elf_Shdr;

typedef struct
{
	Elf64_Word      st_name;
	unsigned char   st_info;
	unsigned char   st_other;
	Elf64_Half      st_shndx;
	Elf64_Addr      st_value;
	Elf64_Qword     st_size;
} Elf64_Sym, Elf_Sym;

typedef struct {
	Elf64_Addr      r_offset;
	Elf64_Qword     r_info;
} Elf64_Rel, Elf_Rel;

typedef struct {
	Elf64_Addr      r_offset;
	Elf64_Qword     r_info;
	Elf64_SQword    r_addend;
} Elf64_Rela, Elf_Rela;

typedef struct {
	Elf64_Qword d_tag;
	union {
		Elf64_Qword     d_val;
		Elf64_Addr      d_ptr;
	} d_un;
} Elf64_Dyn, Elf_Dyn;

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
	DT_GNU_HASH = 0x6ffffef5,
};

#ifdef __x86_64__
enum
{
	R_AMD64_NONE,
	R_AMD64_64,
	R_AMD64_PC32,
	R_AMD64_GOT32,
	R_AMD64_PLT32,
	R_AMD64_COPY,
	R_AMD64_GLOB_DAT,
	R_AMD64_JUMP_SLOT,
	R_AMD64_RELATIVE,
	R_AMD64_GOTPCREL,
	R_AMD64_32,
	R_AMD64_32S,
	R_AMD64_16,
	R_AMD64_PC16,
	R_AMD64_8,
	R_AMD64_PC8,
	R_AMD64_PC64 = 24,
	R_AMD64_GOTOFF64 = 25,
	R_AMD64_GOTPC32 = 26,
	R_AMD64_SIZE32 = 32,
	R_AMD64_SIZE64 = 33,
};
#endif
