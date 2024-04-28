/*
	oboskrnl/driver_interface/loader.cpp
	
	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>
#include <driver_interface/loader.h>

#include <elf/elf64.h>

#include <arch/driver_interface_load.h>
#include <arch/thr_context_info.h>

#include <scheduler/thread.h>
#include <scheduler/scheduler.h>
#include <scheduler/init.h>

#include <vmm/init.h>

#if OBOS_ARCHITECTURE_BITS != 64
#error Cannot compile loader64.cpp when OBOS_ARCHITECTURE_BITS != 64.
#endif

namespace obos
{
	namespace driverInterface
	{
		using namespace elf;
		driverIdList g_driverTable[(int)driverType::MaxValue];
		driverIdList g_drivers;
		uint32_t driverId::nextDriverId;
#define Ehdr(ptr) ((const Elf64_Ehdr*)ptr)
#define OffsetPtr(ptr, off) ((decltype(ptr))(((char*)(ptr)) + (off)))
#define OffsetOf(type, member) ((uintptr_t)&(((type*)nullptr)->member))
		static const char* getElfString(const Elf64_Ehdr* elfHeader, uintptr_t index)
		{
			const char* startAddress = reinterpret_cast<const char*>(elfHeader);
			Elf64_Shdr* stringTable = reinterpret_cast<Elf64_Shdr*>(const_cast<char*>(startAddress) + elfHeader->e_shoff);
			stringTable += elfHeader->e_shstrndx;
			return startAddress + stringTable->sh_offset + index;
		}
		const driverHeader* findDriverHeader(const void* _data, size_t)
		{
			const Elf64_Ehdr* ehdr = Ehdr(_data);
			const Elf64_Shdr* sectionHeaders = OffsetPtr((const Elf64_Shdr*)_data, ehdr->e_shoff);
			const Elf64_Shdr* driverHeaderSection = nullptr;
			for (size_t i = 0; i < ehdr->e_shnum; i++)
			{
				if (strcmp(getElfString(ehdr, sectionHeaders[i].sh_name), OBOS_DRIVER_HEADER_SECTION))
				{
					driverHeaderSection = &sectionHeaders[i];
					break;
				}
			}
			if (driverHeaderSection)
				return OffsetPtr((const driverHeader*)_data, driverHeaderSection->sh_offset);
			return nullptr;
		}
		const Elf64_Shdr* findDriverHeaderSection(const void* _data, size_t)
		{
			const Elf64_Ehdr* ehdr = Ehdr(_data);
			const Elf64_Shdr* sectionHeaders = OffsetPtr((const Elf64_Shdr*)_data, ehdr->e_shoff);
			for (size_t i = 0; i < ehdr->e_shnum; i++)
				if (strcmp(getElfString(ehdr, sectionHeaders[i].sh_name), OBOS_DRIVER_HEADER_SECTION))
					return &sectionHeaders[i];
			return nullptr;
		}
		struct tables
		{
			const Elf64_Shdr* symtab_section;
			const Elf64_Shdr* strtab_section;
		};
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
		const struct driverHeader* VerifyDriver(const void* data, size_t size)
		{
			if (size < sizeof(Elf64_Ehdr))
				return nullptr;
			if (!data)
				return nullptr;
			// Verify the ELF header.
			const Elf64_Ehdr* header = Ehdr(data);
			if (header->e_ident[EI_MAG0] != ELFMAG0 ||
				header->e_ident[EI_MAG1] != ELFMAG1 ||
				header->e_ident[EI_MAG2] != ELFMAG2 ||
				header->e_ident[EI_MAG3] != ELFMAG3)
				return nullptr;
			if (header->e_ident[EI_CLASS] != ELFCLASS64)
				return nullptr;
			uint8_t ei_data_val = 0;
			if (arch::g_endianness == arch::endianness::LittleEndian)
				ei_data_val = ELFDATA2LSB;
			else if (arch::g_endianness == arch::endianness::BigEndian)
				ei_data_val = ELFDATA2MSB;
			else
				ei_data_val = ELFDATANONE;
			if (header->e_ident[EI_DATA] != ei_data_val)
				return nullptr;
#ifdef __x86_64__
			if (header->e_machine != EM_X86_64)
				return nullptr;
#else
#error TODO: Add current architecture
#endif
			if (header->e_type != ET_DYN)
				return nullptr;
			// Verify the driver has exported symbols.
			tables driver_tables = GetDriverSymbolStringTables((uint8_t*)data);
			if (!driver_tables.strtab_section || !driver_tables.symtab_section)
				return nullptr;
			// Verify the driver header.
			auto dheader_section = findDriverHeaderSection(data, size);
			if (!dheader_section)
				return nullptr;
			if (!(dheader_section->sh_flags & 1 /*SHF_WRITE*/))
				return nullptr;
			auto dheader = OffsetPtr((driverHeader*)data, dheader_section->sh_offset);
			if (dheader->magic != g_driverHeaderMagic)
				return nullptr;
			if (dheader->type <= driverType::Invalid || dheader->type > driverType::MaxValue)
				return nullptr;
			// Nothing incorrect has been detected for the driver's file.
			return dheader;
		}

		driverId* LoadDriver(const void* data, size_t size)
		{
			const driverHeader* header = VerifyDriver(data, size);
			if (!header)
				return nullptr;
			// Hand the loading part to the architecture-specific code.
			void *base = arch::LoadDynamicElfFile(data, size);
			// Initialize the driver id.
			driverId* driver = new driverId{};
			// Initialize the driver's symbol table.
			tables driver_tables = GetDriverSymbolStringTables((uint8_t*)data);
			const Elf64_Sym* symbolTable = OffsetPtr((const Elf64_Sym*)data, driver_tables.symtab_section->sh_offset);
			size_t szSymbolTable = driver_tables.symtab_section->sh_size;
			const char* stringTable = OffsetPtr((const char*)data, driver_tables.strtab_section->sh_offset);
			size_t countEntries = szSymbolTable / sizeof(Elf64_Sym);
			for (size_t i = 0; i < countEntries; i++)
			{
				auto& symbol = symbolTable[i];
				if (symbol.st_name >= driver_tables.strtab_section->sh_size)
					continue;
				if ((symbol.st_info >> 4) != STB_GLOBAL)
					continue;
				auto stt = (symbol.st_info & 0xf);
				if (stt != STT_FUNC && stt != STT_OBJECT)
					continue;
				driverSymbol sym{};
				sym.address = (uintptr_t)base + symbol.st_value;
				sym.name = OffsetPtr(stringTable, symbol.st_name);
				sym.type = stt == STT_FUNC ? driverSymbol::SYMBOL_FUNC : driverSymbol::SYMBOL_VARIABLE;
				driver->symbols.push_back(sym);
			}
			auto dheader_section = findDriverHeaderSection(data, size);
			driverHeader* rwHeader = OffsetPtr((driverHeader*)base, dheader_section->sh_addr);
			// Initialize the miscellaneous fields.
			driver->header = rwHeader;
			driver->id = driverId::nextDriverId++;
			driver->driverBaseAddress = base;
			driver->driverEntry = OffsetPtr((void(*)())base, Ehdr(data)->e_entry);
			// Initialize the driver's header.
			rwHeader->id = driver->id;
			// We've done what we need to, append the driver to its appropriate lists.
			g_drivers.Append(driver);
			g_driverTable[(int)driver->header->type - 1].Append(driver);
			return driver;
		}
		bool StartDriver(uint32_t id)
		{
			driverIdNode* node = g_drivers.Find(id);
			if (!node)
				return false;
			driverId* driver = node->data;
			scheduler::Thread* thr = new scheduler::Thread{};
			thr->tid = 0;
			thr->referenceCount = 0;

			thr->priority = scheduler::ThreadPriority::High;
			thr->ogAffinity = scheduler::g_defaultAffinity;

			thr->affinity = thr->ogAffinity;
			thr->status = scheduler::ThreadStatus::CanRun;

			thr->addressSpace = &vmm::g_kernelContext;
			arch::SetupThreadContext(&thr->context, &thr->thread_stack, (uintptr_t)driver->driverEntry, (uintptr_t)driver, false, 0x1'0000, thr->addressSpace);
			scheduler::g_threadPriorities[(int)thr->priority].Append(thr);
			scheduler::yield();
			return true;
		}
		bool UnloadDriver(uint32_t)
		{
			return false;
		}
#define removeNode(head,tail,nNodes,node) \
do {\
	if (node)\
	{\
		if (node->prev)\
			node->prev->next = node->next;\
		if (node->next)\
			node->next->prev = node->prev;\
		if (head == node)\
			head = node;\
		if (tail == node)\
			tail = node;\
		nNodes--;\
	}\
} while (0)
		void driverIdList::Append(driverId* id)
		{
			if (!id)
				return;
			driverIdNode* node = new driverIdNode{ nullptr,nullptr,id };
			if (tail)
				tail->next = node;
			if (!head)
				head = node;
			node->prev = tail;
			tail = node;
			nNodes++;
		}
		void driverIdList::Remove(driverId* id)
		{
			if (!id)
				return;
			driverIdNode* node = Find(id);
			removeNode(head,tail,nNodes, node);
		}
		void driverIdList::Remove(uint32_t id)
		{
			if (!id)
				return;
			driverIdNode* node = Find(id);
			removeNode(head, tail, nNodes, node);
		}
		driverIdNode* driverIdList::Find(driverId* id)
		{
			for (driverIdNode* node = head; node; )
			{
				if (node->data == id)
					return node;
				node = node->next;
			}
			return nullptr;
		}
		driverIdNode* driverIdList::Find(uint32_t id)
		{
			for (driverIdNode* node = head; node; )
			{
				if (node->data->id == id)
					return node;
				node = node->next;
			}
			return nullptr;
		}
	}
}