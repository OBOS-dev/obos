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
			const char* name = nullptr;
			uintptr_t address = 0;
			enum
			{
				SYMBOL_INVALID,
				SYMBOL_FUNC,
				SYMBOL_VARIABLE,
			} type = SYMBOL_INVALID;
		};
		struct driverId
		{
			static uint32_t nextDriverId;
			uint32_t id = 0;
			// This member is set to the header section of the driver, thus it can be modified to set driver header values.
			driverHeader* header = nullptr;
			// Please don't modify this after loading.
			utils::Vector<driverSymbol> symbols{};
			void* driverBaseAddress = nullptr;
			void(*driverEntry)() = nullptr;
			// Should be set by the driver when it is done initializing itself.
			// If the driver's entry is never called, this will likely stay false.
			// Make sure this is true before calling any driver functions.
			bool isDriverInitialized = false;
		};
		struct driverIdNode
		{
			driverIdNode *next = nullptr, *prev = nullptr;
			driverId* data = nullptr;
		};
		struct driverIdList
		{
			driverIdNode *head = nullptr, *tail = nullptr;
			size_t nNodes = 0;
			void Append(driverId*);
			void Remove(driverId*);
			void Remove(uint32_t);
			driverIdNode* Find(driverId*);
			driverIdNode* Find(uint32_t);
		};
		extern driverIdList g_driverTable[(int)driverType::MaxValue];
		extern driverIdList g_drivers;
	}
}