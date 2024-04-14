/*
	oboskrnl/driver_interface/loader.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace driverInterface
	{
		const struct driverHeader* VerifyDriver(const void* data, size_t size);
		struct driverId* LoadDriver(const void* data, size_t size);
		bool UnloadDriver(struct driverId* driver);
	}
}