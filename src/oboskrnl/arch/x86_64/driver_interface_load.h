/*
	oboskrnl/arch/x86_64/driver_interface_load.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace arch
	{
		/// <summary>
		/// Loads a dynamic elf file, applying any relocations.
		/// </summary>
		/// <param name="data">The elf file's data.</param>
		/// <param name="size">The elf file's size.</param>
		/// <returns>The module's base on success, otherwise nullptr.</returns>
		void* LoadDynamicElfFile(const void* data, size_t size);
	}
}