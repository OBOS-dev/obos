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

#include <arch/x86_64/irq/idt.h>

#include <limine/limine.h>

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
	void RegisterExceptionHandlers();
	volatile limine_framebuffer_request framebuffer_request = {
		.id = LIMINE_FRAMEBUFFER_REQUEST,
		.revision = 0
	};
}

using namespace obos;

LIMINE_BASE_REVISION(1);

extern "C" void _start()
{
	g_consoleFont = (void*)font_bin;
	Framebuffer initFB;
	memzero(&initFB, sizeof(initFB));
	if (framebuffer_request.response->framebuffer_count)
	{
		initFB.address = framebuffer_request.response->framebuffers[0]->address;
		initFB.bpp = framebuffer_request.response->framebuffers[0]->bpp;
		initFB.height = framebuffer_request.response->framebuffers[0]->height;
		initFB.width = framebuffer_request.response->framebuffers[0]->width;
		initFB.pitch = framebuffer_request.response->framebuffers[0]->pitch;
		initFB.format = FramebufferFormat::FB_FORMAT_RGBX8888;
	}
	new (&g_kernelConsole) Console{};
	g_kernelConsole.Initialize(&initFB, nullptr, true);
	logger::log("%s: Initializing boot GDT.\n", __func__);
	InitBootGDT();
	logger::log("%s: Initializing IDT.\n", __func__);
	InitializeIDT();
	logger::log("%s: Registering exception handlers.\n", __func__);
	RegisterExceptionHandlers();
	while (1);
}

[[nodiscard]] void* operator new(size_t, void* ptr) noexcept { return ptr; }
[[nodiscard]] void* operator new[](size_t, void* ptr) noexcept { return ptr; }
void operator delete(void*, void*) noexcept {}
void operator delete[](void*, void*) noexcept {}