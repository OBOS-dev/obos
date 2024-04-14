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
		enum class driverType
		{
			Invalid = 0,
			FilesystemDriver,
			DeviceDriver,
			NicDriver,
			DriverLoader,
			KernelExtension,
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
			struct
			{
				acpi_hid hid[8];
				acpi_hid cid[16];
			} acpi_pnp;
			// Whether the specified ACPI table exists
			union
			{
				char signature_str[4];
				uint32_t signature_int;
			} acpi_table;
		};
		// For the kernel to recognize the file as a file, some conditions must be met:
		// A driver header exists on a 8-byte boundary, with the magic field set to g_driverHeaderMagic.
		// The driver header must have all fields valid.
		// The driver must be a dynamic elf file, matching the ABI's specifications for ELF files.
		// Example:
		// The x86_64 SystemV ABI specifies that ELF files must have e_ident[EI_CLASS] = ELFCLASS64, e_ident[EI_DATA] = ELFDATA2LSB, and e_machine = EM_X86_64 (62)
		// If you are unsure on this, run readelf -h on the ELF file to verify the values are what the kernel expects.
		// Make sure the driver is compiled with the same ABI as the kernel, or the kernel won't be able to call the driver.
		// For example, the driver can't be using the Microsoft ABI when the kernel uses the SystemV ABI.
		struct driverHeader
		{
			alignas(0x8) uint64_t magic = g_driverHeaderMagic;
			alignas(0x8) driverType type = driverType::Invalid;
			alignas(0x8) char friendlyName[33] = {};
			// The path of driver to detect whether this driver should be loaded.
			// The "loader" driver.
			// This can be the kernel path.
			// If this is empty, this driver must be loaded manually.
			alignas(0x8) char requestedLoader[257] = {};
			alignas(0x8) void* loaderPacket; // The data expected by the loader driver to know whether this driver should be loaded or not.
			
			alignas(0x8) const char* path = nullptr; // Initialized by the kernel.
			alignas(0x8) const char* loader = nullptr; // Initialized by the kernel.
		};
	}
}