/*
	oboskrnl/arch/x86_64/idt.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <struct_packing.h>

#include <arch/x86_64/irq/idt.h>

extern "C" char __b_isr_handler;
extern "C" char __e_isr_handler;

namespace obos
{
	struct idtEntry
	{
		uint16_t offset1;
		uint16_t selector;
		uint8_t ist;
		uint8_t typeAttributes;
		uint16_t offset2;
		uint32_t offset3;
		uint32_t resv1;
	} g_idtEntries[256];
	struct idtPointer
	{
		uint16_t size;
		uintptr_t idt;
	} OBOS_PACK;
	enum
	{
		// Present, Trap Gate
		DEFAULT_TYPE_ATTRIBUTE = 0x8E,
		// Max DPL: 3
		TYPE_ATTRIBUTE_USER_MODE = 0x60
	};
	uintptr_t g_handlers[256];
	extern "C" void idtFlush(idtPointer* idtptr);
	static void RegisterISRInIDT(uint8_t vec, uintptr_t addr, bool canUsermodeCall, uint8_t ist = 0)
	{
		auto& idtEntry = g_idtEntries[vec];
		idtEntry.ist = ist & 0x7;
		idtEntry.offset1 = addr & 0xffff;
		idtEntry.selector = 0x8;
		idtEntry.typeAttributes = DEFAULT_TYPE_ATTRIBUTE | (canUsermodeCall ? TYPE_ATTRIBUTE_USER_MODE : 0);
		idtEntry.offset2 = (addr >> 16) & 0xffff;
		idtEntry.offset3 = (addr >> 32) & 0xffffffff;
	}
	static int getIntIST(int i)
	{
		if (i > 32)
			return 0;
		if (i == 8)
			return 2;
		return (i == 14 || i == 2 || i == 8 || i == 1 || i == 18 || i == 13 || i == 3) ? 1 : 0;
	}
	void InitializeIDT()
	{
		for (int i = 0; i < 256; i++)
			RegisterISRInIDT(i, (uintptr_t)(&__b_isr_handler + (i * 32)), true, getIntIST(i));
		idtPointer idtPtr{ sizeof(g_idtEntries) - 1, (uintptr_t)g_idtEntries };
		idtFlush(&idtPtr);
	}
	void RawRegisterInterrupt(uint8_t vec, uintptr_t f)
	{
		g_handlers[vec] = f;
	}
}