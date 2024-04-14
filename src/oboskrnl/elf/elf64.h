/*
	oboskrnl/elf/elf64.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace elf
	{
		constexpr uint8_t ELFMAG0 = 0x7f;
		constexpr uint8_t ELFMAG1 = 'E';
		constexpr uint8_t ELFMAG2 = 'L';
		constexpr uint8_t ELFMAG3 = 'F';

		constexpr uint8_t ELFCLASSNONE = 0;
		constexpr uint8_t ELFCLASS32 = 1;
		constexpr uint8_t ELFCLASS64 = 2;

		constexpr uint8_t ELFDATANONE = 0;
		constexpr uint8_t ELFDATA2LSB = 1;
		constexpr uint8_t ELFDATA2MSB = 2;

		constexpr uint8_t EV_NONE = 0;
		constexpr uint8_t EV_CURRENT = 1;

		constexpr uint32_t EI_NIDENT = 16;
		constexpr uint32_t EI_MAG0 = 0;
		constexpr uint32_t EI_MAG1 = 1;
		constexpr uint32_t EI_MAG2 = 2;
		constexpr uint32_t EI_MAG3 = 3;
		constexpr uint32_t EI_CLASS = 4;
		constexpr uint32_t EI_DATA = 5;
		// Must be EV_CURRENT.
		constexpr uint32_t EI_VERSION = 6;
		constexpr uint32_t EI_PAD = 7;

		constexpr uint32_t PF_R = 0x4;
		constexpr uint32_t PF_W = 0x2;
		constexpr uint32_t PF_X = 0x1;

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
			EM_X86_64 = 62,
			// TODO: Add more machine types.
		};

		struct Elf64_Ehdr
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
		};

		struct Elf64_Phdr
		{
			Elf64_Word p_type;
			Elf64_Word p_flags;
			Elf64_Off  p_offset;
			Elf64_Addr p_vaddr;
			Elf64_Addr p_paddr;
			Elf64_Qword p_filesz;
			Elf64_Qword p_memsz;
			Elf64_Qword p_align;
		};

		struct Elf64_Shdr
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
		};

		struct Elf64_Sym
		{
			Elf64_Word      st_name;
			unsigned char   st_info;
			unsigned char   st_other;
			Elf64_Half      st_shndx;
			Elf64_Addr      st_value;
			Elf64_Qword     st_size;
		};

		typedef struct {
			Elf64_Addr      r_offset;
			Elf64_Qword     r_info;
		} Elf64_Rel;

		typedef struct {
			Elf64_Addr      r_offset;
			Elf64_Qword     r_info;
			Elf64_SQword    r_addend;
		} Elf64_Rela;

		typedef struct {
			Elf64_Qword d_tag;
			union {
				Elf64_Qword     d_val;
				Elf64_Addr      d_ptr;
			} d_un;
		} Elf64_Dyn;

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
	}
}