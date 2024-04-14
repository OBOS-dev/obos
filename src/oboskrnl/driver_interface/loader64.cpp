/*
	oboskrnl/driver_interface/loader.cpp
	
	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <elf/elf64.h>

#include <arch/driver_interface_load.h>

#if OBOS_ARCHITECTURE_BITS != 64
#error Cannot compile loader64.cpp when OBOS_ARCHITECTURE_BITS != 64.
#endif

namespace obos
{
	namespace driverInterface
	{
		using namespace elf;
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
		const driverHeader* findDriverHeader(const void* _data, size_t size)
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
		const struct driverHeader* VerifyDriver(const void* data, size_t size)
		{
			if (size < sizeof(Elf64_Ehdr))
				return nullptr;
			if (!data)
				return nullptr;
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
			auto dheader = findDriverHeader(header, size);
			if (!dheader)
				return nullptr;
			if (dheader->magic != g_driverHeaderMagic)
				return nullptr;
			if (dheader->type == driverType::Invalid)
				return nullptr;
			// Nothing incorrect has been detected for the driver's file.
			return dheader;
		}
		driverId* LoadDriver(const void* data, size_t size)
		{
			const driverHeader* header = VerifyDriver(data, size);
			if (!header)
				return nullptr;
			void *base = arch::LoadDynamicElfFile(data, size);
			void(*entry)() = OffsetPtr((void(*)())base, Ehdr(data)->e_entry);
			entry();
			return nullptr;
			// Hand the loading part to the architecture-specific code.
		}
		bool UnloadDriver(driverId* driver) 
		{
			
		}
	}
}