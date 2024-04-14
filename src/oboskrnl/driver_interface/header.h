/*
	oboskrnl/driver_interface/header.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace driverInterface
	{
		constexpr uint64_t g_driverHeaderMagic = 0x0002'7855'0650'CDAA;
#define OBOS_DRIVER_HEADER_SECTION ".obosDriverHeader"
#ifndef __INTELLISENSE__
#	define OBOS_DEFINE_IN_SECTION(s) __attribute__((section(s)))
#else
#	define OBOS_DEFINE_IN_SECTION(section)
#endif
		enum class driverType
		{
			Invalid = 0,
			FilesystemDriver,
			DeviceDriver,
			NicDriver,
			DriverLoader,
			KernelExtension,
			MaxValue = KernelExtension,
			
		};
		struct kernelLoaderPacket
		{
			struct acpi_hid
			{
				char id[8];
				operator char*()
				{ return id; }
			};
			// Bit 0: PNP
			// Bit 1: Whether the specified ACPI table exists
			uint32_t howToIdentify = 0;
			// PNP
			struct __acpi_pnp
			{
				static constexpr size_t maxHIDs = 4;
				static constexpr size_t maxCIDs = 16;
				acpi_hid hid[maxHIDs] = {};
				acpi_hid cid[maxCIDs] = {};
				size_t nHIDs = 0;
				size_t nCIDs = 0;
			} acpi_pnp;
			// Whether the specified ACPI table exists
			union
			{
				char signature_str[4];
				uint32_t signature_int;
			} acpi_table;
		};
		// For the kernel to recognize the file as a file, some conditions must be met:
		// A driver header exists in the section specified by the macro OBOS_DRIVER_HEADER_SECTION, with the magic field set to g_driverHeaderMagic. The section must be at least RW.
		// The driver header must have all fields valid.
		// The driver must have its symbols exported.
		// The driver must be a dynamic elf file, matching the ABI's specifications for ELF files.
		// Example:
		// The x86_64 SystemV ABI specifies that ELF files must have e_ident[EI_CLASS] = ELFCLASS64, e_ident[EI_DATA] = ELFDATA2LSB, and e_machine = EM_X86_64 (62)
		// If you are unsure on this, run readelf -h on the ELF file to verify the values are what the kernel expects.
		// Make sure the driver is compiled with the same ABI as the kernel, or the kernel won't be able to call the driver.
		// For example, the driver can't be using the Microsoft ABI when the kernel uses the SystemV ABI.
		struct driverHeader
		{
			uint64_t magic = g_driverHeaderMagic;
			driverType type = driverType::Invalid;
			char friendlyName[33] = {};
			// The path of driver to detect whether this driver should be loaded.
			// The "loader" driver.
			// This can be the kernel path.
			// If this is empty, this driver must be loaded manually.
			char requestedLoader[257] = {};
			void* loaderPacket; // The data expected by the loader driver to know whether this driver should be loaded or not.
			
			const char* path = nullptr; // Initialized by the kernel.
			const char* loader = nullptr; // Initialized by the kernel.
			uint32_t id; // Initialized by the kernel.
		};
	}
}