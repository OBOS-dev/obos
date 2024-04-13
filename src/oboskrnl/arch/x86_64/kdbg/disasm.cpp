/*
	oboskrnl/arch/x86_64/kdbg/disasm.cpp

	Copyright (c) 2024 Omar Berrow
*/

#if OBOS_KDBG_ENABLED
#include <int.h>

#include <arch/x86_64/kdbg/io.h>

#include <ZyDis.h>

#include <vmm/init.h>
#include <vmm/mprot.h>

#include <elf/elf64.h>

#include <limine/limine.h>

#define TO_EHDR(obj) ((elf::Elf64_Ehdr*)obj)
#define TO_STRING_TABLE(ehdr, stable, off) ((const char*)ehdr + stable + off)

namespace obos
{
	const elf::Elf64_Shdr* getSectionHeader(const elf::Elf64_Ehdr* ehdr, const char* name);
	extern volatile limine_kernel_file_request kernel_file;
	extern volatile limine_kernel_address_request kernel_addr;
	namespace kdbg
	{
		static void addr2sym(uintptr_t rip, const char** const symbolName, uintptr_t* symbolBase, size_t* symbolSize, uint8_t symType)
		{
			if (rip < kernel_addr.response->virtual_base)
			{
				*symbolName = nullptr;
				*symbolBase = 0;
				*symbolSize = 0;
				return;
			}
			uintptr_t base = (uintptr_t)kernel_file.response->kernel_file->address;
			elf::Elf64_Ehdr* ehdr = (elf::Elf64_Ehdr*)base;
			size_t stable = 0;
			if (getSectionHeader(ehdr, ".strtab"))
				stable = getSectionHeader(ehdr, ".strtab")->sh_offset;
			const elf::Elf64_Shdr* symtab = getSectionHeader(ehdr, ".symtab");
			if (!symtab)
			{
				*symbolName = nullptr;
				*symbolBase = 0;
				return;
			}
			size_t nEntries = symtab->sh_size / symtab->sh_entsize;
			for (size_t i = 0; i < nEntries; i++)
			{
				elf::Elf64_Sym* symbol = (elf::Elf64_Sym*)(base + symtab->sh_offset + i*symtab->sh_entsize);
				if (symType != elf::STT_NOTYPE)
					if ((symbol->st_info & 0xf) != symType)
						continue;
				if (rip >= symbol->st_value && rip < (symbol->st_value + symbol->st_size))
				{
					if (stable)
						*symbolName = TO_STRING_TABLE(ehdr, stable, symbol->st_name);
					else
						*symbolName = "no strtab";
					*symbolBase = symbol->st_value;
					*symbolSize = symbol->st_size;
					return;
				}
			}
		}
		bool disasm(void* at, size_t nInstructions)
		{
			ZyanU64 eip = reinterpret_cast<ZyanU64>(at);
	
			uint8_t* data = (uint8_t*)at;
	
			ZyanUSize offset = 0;
			size_t bytesSinceLastSymbolCheck = 0;
			ZydisDisassembledInstruction instruction;
	
			size_t i = 0;
			size_t symSize = 0;
			uintptr_t symBase = 0;
			const char* symName = nullptr;
			
			printf("Disassembly of address 0x%p.\n", at);
			addr2sym((uintptr_t)at, &symName, &symBase, &symSize, elf::STT_FUNC);
			bytesSinceLastSymbolCheck = (uintptr_t)at-symBase;
			if (symName)
				printf("%p <%s>:\n", at, symName);
			while (i < nInstructions)
			{
				if (bytesSinceLastSymbolCheck >= symSize)
				{
					addr2sym((uintptr_t)eip, &symName, &symBase, &symSize, elf::STT_FUNC);
					if (symName)
						printf("\n%p <%s>:\n", (void*)symBase, symName);
					bytesSinceLastSymbolCheck = 0;
				}
				ZyanStatus st = ZydisDisassembleIntel(
					ZYDIS_MACHINE_MODE_LONG_64,
					eip,
					data + offset,
					32,
					&instruction
				);
				// TODO: Implement translating memory addresses to symbol names.
				if (ZYAN_SUCCESS(st))
					printf("%p: %s\n", eip, instruction.text);
				else
				{
					printf("%p: ???\n", eip);
					instruction.info.length = 1;
				}
				offset += instruction.info.length;
				bytesSinceLastSymbolCheck += instruction.info.length;
				eip += instruction.info.length;
				i++;
			}
			return true;
		}
	}
}
#else
// Nothing to do
#endif