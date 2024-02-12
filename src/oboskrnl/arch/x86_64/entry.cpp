/*
	oboskrnl/arch/x86_64/entry.cpp
	
	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <console.h>
#include <klog.h>
#include <memmanip.h>

#include <arch/x86_64/font.h>

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

namespace obos
{
	ultra_boot_context* g_bootContext;
	uintptr_t g_higherHalfBase;
	uintptr_t g_rsdp;
	ultra_memory_map_entry* g_physMMAP;
	size_t g_nPhysMMAPEntries;
}

using namespace obos;

extern "C" void _start(ultra_boot_context* bcontext, uint32_t magic)
{
	if (magic != 0x554c5442)
		return;
	g_bootContext = bcontext;
	g_consoleFont = (void*)font_bin;
	Framebuffer initFB;
	memzero(&initFB, sizeof(initFB));
	for (size_t i = 0, off = 0; i < g_bootContext->attribute_count; i++)
	{
		ultra_attribute_header* hdr = (ultra_attribute_header*)((uint8_t*)g_bootContext->attributes + off);
		switch (hdr->type)
		{
		case ULTRA_ATTRIBUTE_PLATFORM_INFO:
		{
			ultra_platform_info_attribute* info = (ultra_platform_info_attribute*)hdr;
			g_higherHalfBase = info->higher_half_base;
			g_rsdp = info->acpi_rsdp_address;
			break;
		}
		case ULTRA_ATTRIBUTE_FRAMEBUFFER_INFO:
		{
			ultra_framebuffer_attribute* fb = (ultra_framebuffer_attribute*)hdr;
			initFB.address = (void*)(fb->fb.physical_address + g_higherHalfBase);
			initFB.format = (FramebufferFormat)fb->fb.format;
			initFB.width = fb->fb.width;
			initFB.height = fb->fb.height;
			initFB.pitch = fb->fb.pitch;
			initFB.bpp = fb->fb.bpp;
			break;
		}
		case ULTRA_ATTRIBUTE_MEMORY_MAP:
		{
			ultra_memory_map_attribute* mmap = (ultra_memory_map_attribute*)hdr;
			g_nPhysMMAPEntries = ULTRA_MEMORY_MAP_ENTRY_COUNT(*hdr);
			g_physMMAP = mmap->entries;
			break;
		}
		default:
			break;
		}
		off += hdr->size;
	}
	new (&g_kernelConsole) Console{};
	g_kernelConsole.Initialize(&initFB, nullptr, true);
	logger::log("%s: Initializing boot GDT.\n", __func__);
	InitBootGDT();
	while (1);
}

[[nodiscard]] void* operator new(size_t, void* ptr) noexcept { return ptr; }
[[nodiscard]] void* operator new[](size_t, void* ptr) noexcept { return ptr; }
void operator delete(void*, void*) noexcept {}
void operator delete[](void*, void*) noexcept {}