/*
	oboskrnl/driver_interface/driverId.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <driver_interface/header.h>

namespace obos
{
	namespace driverInterface
	{
		struct driverSymbol
		{
			char* name;
			uintptr_t address;
			bool isFunction;
		};
		struct driverId
		{
			uint32_t id;
			driverHeader* header;
			// TODO: Add symbol list.
		};
	}
}