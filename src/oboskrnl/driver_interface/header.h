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
		constexpr uint32_t g_driverHeaderMagic = 0xD823F54E;
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
			typedef char[8] acpi_hid;
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
		struct driverHeader
		{
			uint32_t magic = g_driverMagic;
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
		};
	}
}