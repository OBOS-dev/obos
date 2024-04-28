/*
	oboskrnl/arch/x86_64/driver_interface_load.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <utils/vector.h>

#include <vmm/map.h>
#include <vmm/init.h>
#include <vmm/prot.h>
#include <vmm/mprot.h>

#include <elf/elf64.h>

#include <limine/limine.h>

#define Ehdr(ptr) ((const Elf64_Ehdr*)ptr)
#define OffsetPtr(ptr, off) ((decltype(ptr))(((char*)(ptr)) + (off)))
#define OffsetOf(type, member) ((uintptr_t)&(((type*)nullptr)->member))

namespace obos
{
	extern volatile limine_kernel_file_request kernel_file;
	namespace arch
	{
		using namespace elf;

		struct relocation
		{
			uint32_t symbolTableOffset = 0;
			uintptr_t virtualAddress = 0;
			uint16_t relocationType = 0;
			int64_t addend = 0;
		};
		struct copy_reloc
		{
			void* src = nullptr, * dest = nullptr;
			size_t size = 0;
		};
		static const char* getElfString(const Elf64_Ehdr* elfHeader, uintptr_t index)
		{
			const char* startAddress = reinterpret_cast<const char*>(elfHeader);
			Elf64_Shdr* stringTable = reinterpret_cast<Elf64_Shdr*>(const_cast<char*>(startAddress) + elfHeader->e_shoff);
			stringTable += elfHeader->e_shstrndx;
			return startAddress + stringTable->sh_offset + index;
		}
		static uint32_t ElfHash(const char* name)
		{
			uint32_t h = 0, g = 0;

			while (*name)
			{
				h = (h << 4) + *name++;
				if ((g = h & 0xf0000000))
					h ^= g >> 24;
				h &= ~g;
			}
			return h;
		}
		Elf64_Sym* GetSymbolFromTable(
			uint8_t* fileStart,
			uint8_t* baseAddress,
			Elf64_Sym* symbolTable,
			uintptr_t hashTableOff,
			Elf64_Off stringTable,
			const char* _symbol)
		{
			Elf64_Word* hashTableBase = (Elf64_Word*)(baseAddress + hashTableOff);
			auto& nBuckets = hashTableBase[0];
			auto currentBucket = ElfHash(_symbol) % nBuckets;
			Elf64_Word* firstBucket = hashTableBase + 2;
			Elf64_Word* firstChain = firstBucket + nBuckets;
			size_t index = firstBucket[currentBucket];
			while (index)
			{
				auto& symbol = symbolTable[index];
				const char* symbolName = (char*)(fileStart + stringTable + symbol.st_name);
				if (strcmp(symbolName, _symbol))
					return &symbol;

				index = firstChain[index];
			}
			return nullptr;
		}
		static Elf64_Sym* GetSymbolFromTable(
			uint8_t* fileStart,
			Elf64_Sym* symbolTable,
			size_t szSymbolTable,
			Elf64_Off stringTable,
			const char* _symbol,
			uint8_t requiredBinding = STB_GLOBAL)
		{
			size_t countEntries = szSymbolTable / sizeof(Elf64_Sym);
			for (size_t i = 0; i < countEntries; i++)
			{
				auto& symbol = symbolTable[i];
				if ((symbol.st_info >> 4) != requiredBinding)
					continue;
				const char* symbolName = (char*)(fileStart + stringTable + symbol.st_name);
				if (strcmp(symbolName, _symbol))
					return &symbol;
			}
			return nullptr;
		}
		struct tables
		{
			const Elf64_Shdr* symtab_section;
			const Elf64_Shdr* strtab_section;
		};
		static tables GetKernelSymbolStringTables()
		{
			uintptr_t kernelFileStart = (uintptr_t)kernel_file.response->kernel_file->address;
			Elf64_Ehdr* eheader = (Elf64_Ehdr*)kernelFileStart;
			Elf64_Shdr* iter = reinterpret_cast<Elf64_Shdr*>(kernelFileStart + eheader->e_shoff);
			Elf64_Shdr* symtab_section = nullptr;
			Elf64_Shdr* strtab_section = nullptr;

			for (size_t i = 0; i < eheader->e_shnum; i++, iter++)
			{
				const char* section_name = getElfString(eheader, iter->sh_name);
				if (strcmp(".symtab", section_name))
				{
					symtab_section = iter;
					continue;
				}
				if (strcmp(".strtab", section_name))
				{
					strtab_section = iter;
					continue;
				}
				if (strtab_section != nullptr && symtab_section != nullptr)
					break;
			}
			if (!symtab_section)
				return { nullptr,nullptr };
			if (!strtab_section)
				return { nullptr,nullptr };
			return { symtab_section,strtab_section };
		}
		static tables GetDriverSymbolStringTables(const uint8_t* file)
		{
			const Elf64_Ehdr* eheader = (Elf64_Ehdr*)file;
			const Elf64_Shdr* iter = reinterpret_cast<const Elf64_Shdr*>(file + eheader->e_shoff);
			const Elf64_Shdr* symtab_section = nullptr;
			const Elf64_Shdr* strtab_section = nullptr;

			for (size_t i = 0; i < eheader->e_shnum; i++, iter++)
			{
				const char* section_name = getElfString(eheader, iter->sh_name);
				if (strcmp(".symtab", section_name))
				{
					symtab_section = iter;
					continue;
				}
				if (strcmp(".strtab", section_name))
				{
					strtab_section = iter;
					continue;
				}
				if (strtab_section != nullptr && symtab_section != nullptr)
					break;
			}
			if (!symtab_section)
				return { nullptr,nullptr };
			if (!strtab_section)
				return { nullptr,nullptr };
			return { symtab_section,strtab_section };
		}
		static Elf64_Sym* GetSymbolFromIndex(
			Elf64_Sym* symbolTable,
			size_t index)
		{
			return symbolTable + index;
		}
		static bool ApplyRelocations(const Elf64_Ehdr* ehdr, const Elf64_Dyn* dynamicHeader, void* baseAddress)
		{
			// Most of the code here is copied from the previous kernel.
			void* file = (void*)ehdr;
			utils::Vector<relocation> required_relocations{};
			const Elf64_Dyn* currentDynamicHeader = dynamicHeader;
			size_t last_dtrelasz = 0, last_dtrelsz = 0, last_dtpltrelsz = 0;
			bool awaitingRelaSz = false, foundRelaSz = false;
			const Elf64_Dyn* dynEntryAwaitingRelaSz = nullptr;
			bool awaitingRelSz = false, foundRelSz = false;
			const Elf64_Dyn* dynEntryAwaitingRelSz = nullptr;
			uint64_t last_dlpltrel = 0;
			auto handleDtRel = [&](const Elf64_Dyn* dynamicHeader, size_t sz) {
				const Elf64_Rela* relTable = (Elf64_Rela*)OffsetPtr(file, dynamicHeader->d_un.d_ptr);
				for (size_t i = 0; i < sz / sizeof(Elf64_Rela); i++)
					required_relocations.push_back({
						(uint32_t)(relTable[i].r_info >> 32),
						relTable[i].r_offset,
						(uint16_t)(relTable[i].r_info & 0xffff),
						relTable[i].r_addend,
						});
				};
			auto handleDtRela = [&](const Elf64_Dyn* dynamicHeader, size_t sz) {
				const Elf64_Rela* relTable = (Elf64_Rela*)OffsetPtr(file, dynamicHeader->d_un.d_ptr);
				for (size_t i = 0; i < (sz / sizeof(Elf64_Rela)); i++)
					required_relocations.push_back({
						(uint32_t)(relTable[i].r_info >> 32),
						relTable[i].r_offset,
						(uint16_t)(relTable[i].r_info & 0xffff),
						relTable[i].r_addend,
						});
				};
			[[maybe_unused]] Elf64_Sym* symbolTable = 0;
			[[maybe_unused]] Elf64_Off     stringTable = 0;
			[[maybe_unused]] uintptr_t hashTableOffset = 0;
			[[maybe_unused]] Elf64_Addr* GOT = nullptr;
			for (size_t i = 0; currentDynamicHeader->d_tag != DT_NULL; i++, currentDynamicHeader++)
			{
				switch (currentDynamicHeader->d_tag)
				{
				case DT_HASH:
					hashTableOffset = currentDynamicHeader->d_un.d_ptr;
					break;
				case DT_PLTGOT:
					// TODO: Find out whether this is the PLT or GOT (if possible).
					GOT = (Elf64_Addr*)OffsetPtr(baseAddress, currentDynamicHeader->d_un.d_ptr);
					break;
				case DT_REL:
					if (!foundRelSz)
					{
						awaitingRelSz = true;
						dynEntryAwaitingRelSz = currentDynamicHeader;
						break;
					}
					handleDtRel(currentDynamicHeader, last_dtrelsz);
					foundRelSz = false;
					last_dtrelsz = 0;
					break;
				case DT_RELA:
					if (!foundRelaSz)
					{
						awaitingRelaSz = true;
						dynEntryAwaitingRelaSz = currentDynamicHeader;
						break;
					}
					handleDtRela(currentDynamicHeader, last_dtrelasz);
					foundRelaSz = false;
					last_dtrelasz = 0;
					break;
				case DT_JMPREL:
					switch (last_dlpltrel)
					{
					case DT_REL:
						handleDtRel(currentDynamicHeader, last_dtpltrelsz);
						break;
					case DT_RELA:
						handleDtRela(currentDynamicHeader, last_dtpltrelsz);
						break;
					default:
						break;
					}
					break;
				case DT_RELSZ:
					last_dtrelsz = currentDynamicHeader->d_un.d_val;
					foundRelSz = !awaitingRelSz;
					if (awaitingRelSz)
					{
						handleDtRel(dynEntryAwaitingRelSz, last_dtrelsz);
						awaitingRelSz = false;
					}
					break;
				case DT_RELASZ:
					last_dtrelasz = currentDynamicHeader->d_un.d_val;
					foundRelaSz = !awaitingRelaSz;
					if (awaitingRelaSz)
					{
						handleDtRel(dynEntryAwaitingRelaSz, last_dtrelasz);
						awaitingRelaSz = false;
					}
					break;
				case DT_PLTREL:
					last_dlpltrel = currentDynamicHeader->d_un.d_val;
					break;
				case DT_PLTRELSZ:
					last_dtpltrelsz = currentDynamicHeader->d_un.d_val;
					break;
				case DT_STRTAB:
					stringTable = currentDynamicHeader->d_un.d_ptr;
					break;
				case DT_SYMTAB:
					symbolTable = (Elf64_Sym*)OffsetPtr(baseAddress, currentDynamicHeader->d_un.d_ptr);
					break;
				default:
					break;
				}
			}
			utils::Vector<copy_reloc> copy_relocations;
			uint8_t* kFileBase = (uint8_t*)kernel_file.response->kernel_file->address;
			tables kernel_tables = GetKernelSymbolStringTables();
			for (size_t j = 0; j < required_relocations.length(); j++)
			{
				auto& i = required_relocations.at(j);
				Elf64_Sym* Symbol = nullptr;
				if (i.symbolTableOffset)
				{
					auto& Unresolved_Symbol = *GetSymbolFromIndex(symbolTable, i.symbolTableOffset);
					Symbol = GetSymbolFromTable(
						kFileBase,
						(Elf64_Sym*)(kFileBase + kernel_tables.symtab_section->sh_offset),
						kernel_tables.symtab_section->sh_size,
						kernel_tables.strtab_section->sh_offset,
						(const char*)OffsetPtr(baseAddress, stringTable + Unresolved_Symbol.st_name)
					);
					if (!Symbol)
						return false;
					if (Unresolved_Symbol.st_size != Symbol->st_size && i.relocationType == R_AMD64_COPY)
					{
						// Oh no!
						return false;
					}
				}
				auto& type = i.relocationType;
				uintptr_t relocAddr = (uintptr_t)baseAddress + i.virtualAddress;
				uint64_t relocResult = 0;
				uint8_t relocSize = 0;
				switch (type)
				{
				case R_AMD64_NONE:
					continue;
				case R_AMD64_64:
					relocResult = Symbol->st_value + i.addend;
					relocSize = 8;
					break;
				case R_AMD64_PC32:
					relocResult = Symbol->st_value + i.addend - relocAddr;
					relocSize = 4;
					break;
				case R_AMD64_GOT32:
					logger::warning("%s: Unimplemented: R_AMD64_GOT32 Relocation.\n", __func__);
					return false;
					// TODO: Replace the zero in the calculation with "G" (see elf spec for more info).
					relocResult = 0 + i.addend;
					relocSize = 4;
					break;
				case R_AMD64_PLT32:
					logger::warning("%s: Unimplemented: R_AMD64_PLT32 Relocation.\n", __func__);
					return false;
					// TODO: Replace the zero in the calculation with "L" (see elf spec for more info).
					relocResult = 0 + i.addend - relocAddr;
					relocSize = 4;
					break;
				case R_AMD64_COPY:
					// Save copy relocations for the end because if we don't, it might contain unresolved addresses.
					copy_relocations.push_back({ (void*)relocAddr, (void*)Symbol->st_value, Symbol->st_size });
					relocSize = 0;
					break;
				case R_AMD64_JUMP_SLOT:
				case R_AMD64_GLOB_DAT:
					relocResult = Symbol->st_value;
					relocSize = 8;
					break;
				case R_AMD64_RELATIVE:
					relocResult = (uint64_t)baseAddress + i.addend;
					relocSize = 8;
					break;
				case R_AMD64_GOTPCREL:
					return false;
					// TODO: Replace the zero in the calculation with "G" (see elf spec for more info).
					relocResult = 0 + (uint64_t)baseAddress + i.addend - relocAddr;
					relocSize = 8;
					break;
				case R_AMD64_32:
					relocResult = Symbol->st_value + i.addend;
					relocSize = 4;
					break;
				case R_AMD64_32S:
					relocResult = Symbol->st_value + i.addend;
					relocSize = 4;
					break;
				case R_AMD64_16:
					relocResult = Symbol->st_value + i.addend;
					relocSize = 2;
					break;
				case R_AMD64_PC16:
					relocResult = Symbol->st_value + i.addend - relocAddr;
					relocSize = 2;
					break;
				case R_AMD64_8:
					relocResult = Symbol->st_value + i.addend;
					relocSize = 1;
					break;
				case R_AMD64_PC8:
					relocResult = Symbol->st_value + i.addend - relocAddr;
					relocSize = 1;
					break;
				case R_AMD64_PC64:
					relocResult = Symbol->st_value + i.addend - relocAddr;
					relocSize = 8;
					break;
				case R_AMD64_GOTOFF64:
					relocResult = Symbol->st_value + i.addend - ((uint64_t)GOT);
					relocSize = 8;
					break;
				case R_AMD64_GOTPC32:
					relocResult = (uint64_t)GOT + i.addend + relocAddr;
					relocSize = 8;
					break;
				case R_AMD64_SIZE32:
					relocSize = 4;
					relocResult = Symbol->st_size + i.addend;
					break;
				case R_AMD64_SIZE64:
					relocSize = 8;
					relocResult = Symbol->st_size + i.addend;
					break;
				default:
					break; // Ignore.
				}
				switch (relocSize)
				{
				case 0:
					break; // The relocation type is rather unsupported or special.
				case 1: // word8
					*(uint8_t*)(relocAddr) = (uint8_t)(relocResult & 0xff);
					break;
				case 2: // word16
					*(uint16_t*)(relocAddr) = (uint16_t)(relocResult & 0xffff);
					break;
				case 4: // word32
					*(uint32_t*)(relocAddr) = (uint16_t)(relocResult & 0xffffffff);
					break;
				case 8: // word64
					*(uint64_t*)(relocAddr) = relocResult;
					break;
				default:
					break;
				}
			}
			// Apply copy relocations.
			for (size_t i = 0; i < copy_relocations.length(); i++)
			{
				auto& reloc = copy_relocations.at(i);
				memcpy(reloc.src, reloc.dest, reloc.size);
			}
			return true;
		}
		void* LoadDynamicElfFile(const void* data, size_t size)
		{
			// First, figure out the size of all the program headers.
			void* base = nullptr, *end = nullptr;
			const Elf64_Ehdr* ehdr = Ehdr(data);
			const Elf64_Phdr* programHeaders = OffsetPtr((const Elf64_Phdr*)data, ehdr->e_phoff);
			const Elf64_Phdr* dynamicPhdr = nullptr;
			const Elf64_Dyn*  dynamicHdr = nullptr;
			for (size_t i = 0; i < ehdr->e_phnum; i++)
			{
				if (programHeaders[i].p_type == PT_DYNAMIC)
					dynamicPhdr = &programHeaders[i];
				if (programHeaders[i].p_type != PT_LOAD)
					continue;
				if (!end || programHeaders[i].p_vaddr > (uintptr_t)end)
					end = (void*)programHeaders[i].p_vaddr;
			}
			if (!dynamicPhdr || !end)
				return nullptr;
			size_t progSize = (((size_t)end) + 0xfff) & ~0xfff;
			base = vmm::Allocate(&vmm::g_kernelContext, nullptr, progSize, vmm::FLAGS_GUARD_PAGE_LEFT|vmm::FLAGS_GUARD_PAGE_RIGHT, 0);
			end = OffsetPtr(base, progSize);
			for (size_t i = 0; i < ehdr->e_phnum; i++)
			{
				if (programHeaders[i].p_type != PT_LOAD)
					continue;
				void* phdrBase = (void*)OffsetPtr(base, programHeaders[i].p_vaddr);
				memcpy(phdrBase, OffsetPtr(data, programHeaders[i].p_offset), programHeaders[i].p_filesz);
				if (programHeaders[i].p_memsz - programHeaders[i].p_filesz)
					memzero(OffsetPtr(phdrBase, programHeaders[i].p_filesz), programHeaders[i].p_memsz - programHeaders[i].p_filesz);
			}
			dynamicHdr = OffsetPtr((const Elf64_Dyn*)data, dynamicPhdr->p_offset);
			if (!ApplyRelocations(ehdr, dynamicHdr, base))
			{
				// Uh oh...
				// We couldn't apply relocations on this driver.
				vmm::Free(&vmm::g_kernelContext, base, progSize);
				return nullptr;
			}
			for (size_t i = 0; i < ehdr->e_phnum; i++)
			{
				if (programHeaders[i].p_type != PT_LOAD)
					continue;
				void* phdrBase = (void*)OffsetPtr(base, programHeaders[i].p_vaddr);
				vmm::prot_t prot = 0;
				if (programHeaders[i].p_flags & PF_X)
					prot |= vmm::PROT_EXECUTE;
				if ((programHeaders[i].p_flags & PF_R) && !(programHeaders[i].p_flags & PF_W))
					prot |= vmm::PROT_READ_ONLY;
				vmm::SetProtection(&vmm::g_kernelContext, phdrBase, programHeaders[i].p_memsz, prot);
			}
			return base;
		}
	}
}