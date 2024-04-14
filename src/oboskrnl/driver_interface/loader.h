/*
	oboskrnl/driver_interface/loader.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <export.h>

namespace obos
{
	namespace driverInterface
	{
		/// <summary>
		/// Verifies a file that claims to be a driver.
		/// </summary>
		/// <param name="data">The file's data.</param>
		/// <param name="size">The file size.</param>
		/// <returns>The driver header, or nullptr on failure. This object's lifetime is determined by the lifetime of the 'data' parameter.</returns>
		OBOS_EXPORT const struct driverHeader* VerifyDriver(const void* data, size_t size);
		/// <summary>
		/// Loads a driver into memory.
		/// </summary>
		/// <param name="data">The file's data.</param>
		/// <param name="size">The file size.</param>
		/// <returns>The driver identification, or nullptr on failure. This object's lifetime is controlled by the kernel.</returns>
		OBOS_EXPORT struct driverId* LoadDriver(const void* data, size_t size);
		/// <summary>
		/// Starts a driver by sending it to it's entry point.
		/// </summary>
		/// <param name="id">The driver's id.</param>
		/// <returns>Whether starting the driver succeeded (true) or not (false).</returns>
		OBOS_EXPORT bool StartDriver(uint32_t id);
		/// <summary>
		/// Unloads a driver from memory.
		/// </summary>
		/// <param name="id">The driver's id.</param>
		/// <returns>Whether unloading the driver succeeded (true) or not (false).</returns>
		OBOS_EXPORT bool UnloadDriver(uint32_t id);
	}
}