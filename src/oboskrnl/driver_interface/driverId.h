/*
	oboskrnl/driver_interface/driverId.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <driver_interface/header.h>

#include <utils/vector.h>

namespace obos
{
	namespace driverInterface
	{
		struct driverSymbol
		{
			const char* name;
			uintptr_t address;
			enum
			{
				SYMBOL_FUNC,
				SYMBOL_VARIABLE,
			} type;
		};
		struct driverId
		{
			uint32_t id;
			const driverHeader* header;
			utils::Vector<driverSymbol> symbols;
		};
	}
}