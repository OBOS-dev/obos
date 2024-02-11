/*
	oboskrnl/arch/x86_64/entry.cpp
	
	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>

#include <UltraProtocol/ultra_protocol.h>

extern "C" void InitBootGDT();

struct gdtEntry
{
	uint16_t limitLow;
	uint16_t baseLow;
	uint8_t  baseMiddle1;
	uint8_t  access;
	uint8_t  granularity;
	uint8_t  baseMiddle2;
	uint64_t baseHigh;
} __attribute__((packed));

extern "C" char GDT;

extern "C" void _start()
{
	gdtEntry* tss_entry = (gdtEntry*)(&GDT + 0x18);
	InitBootGDT();
	while (1);
}