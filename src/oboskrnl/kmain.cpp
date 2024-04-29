/*
	oboskrnl/main.cpp
 
	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <uacpi/uacpi.h>
#include <uacpi/sleep.h>

#include <irq/irql.h>

#include <allocators/allocator.h>

#include <driver_interface/loader.h>
#include <driver_interface/driverId.h>

#include <scheduler/thread.h>

#include <vfs/cwd.h>
#include <vfs/index_node.h>
#include <vfs/fsnode.h>

#ifdef __x86_64__
#include <limine/limine.h>
#endif

#define verify_status(st, in) \
if (st != UACPI_STATUS_OK)\
	obos::logger::panic(nullptr, "uACPI Failed in %s! Status code: %d, error message: %s\n", #in, st, uacpi_status_to_string(st));

namespace obos
{
#ifdef __x86_64__
	extern volatile limine_rsdp_request rsdp_request;
	extern volatile limine_hhdm_request hhdm_offset;
	volatile limine_module_request module_request = {
		.id = LIMINE_MODULE_REQUEST,
		.revision = 0
	};
	uint64_t random_number()
	{
		uint64_t rand = 0;
		asm volatile("rdrand %0" : :"r"(rand));
		return rand;
	}
#endif
	size_t runAllocatorTests(allocators::Allocator& allocator, size_t passes)
	{
		logger::debug("%s: Testing allocator. Pass count is %lu.\n", __func__, passes);
		void* lastDiv16Pointer = nullptr;
		size_t passInterval = 10000;
		if (passInterval % 10)
			passInterval += (passInterval - passInterval % 10);
		size_t lastStatusMessageInterval = 0;
		const char* buf = "\xef\xbe\xad\xed";
		size_t lastFreeIndex = 0;
		for (size_t i = 0; i < passes; i++)
		{
			if (!i)
				logger::debug("%s: &i=0x%p\n", __func__, &i);
			if ((lastStatusMessageInterval + passInterval) == i)
			{
				logger::debug("%s: Finished %lu passes so far.\n", __func__, i);
				lastStatusMessageInterval = i;
			}
			uint64_t r = random_number();
			void* mem = allocator.Allocate(r % 0x2000 + 16);
			if (!mem)
				return i;
			((uint8_t*)mem)[i % 4] = buf[i % 4];
			if (++lastFreeIndex == 3)
			{
				lastFreeIndex = 0;
				if (lastDiv16Pointer)
				{
					size_t objSize = allocator.QueryObjectSize(lastDiv16Pointer);
					if (objSize == SIZE_MAX)
						return i;
					allocator.Free(lastDiv16Pointer, objSize);
				}
				lastDiv16Pointer = mem;
			}
		}
		return passes;
	}
	void kmain()
	{
		logger::debug("In %s.\n", __func__);
		// constexpr size_t passes = 1'000'000;
		// size_t passesFinished = runAllocatorTests(*allocators::g_kAllocator, passes);
		// OBOS_ASSERTP(passesFinished == passes, "Allocator tests failed. Passes finished: %lu",, passesFinished);
		logger::log("%s: Initializing uACPI\n", __func__);
		uintptr_t rsdp = 0;
#ifdef __x86_64__
		rsdp = ((uintptr_t)rsdp_request.response->address - hhdm_offset.response->offset);
#endif
		uacpi_init_params params = {
			rsdp,
			{ UACPI_LOG_TRACE, 0 }
		};
		uacpi_status st = uacpi_initialize(&params);
		verify_status(st, uacpi_initialize);

		st = uacpi_namespace_load();
		verify_status(st, uacpi_namespace_load);

		st = uacpi_namespace_initialize();
		verify_status(st, uacpi_namespace_initialize);

		auto setupDirectory = [](const char* name, vfs::index_node* parent)->vfs::index_node*
			{
				if (parent && parent->type != vfs::index_node_type::Directory)
					return nullptr;
				vfs::index_node* node = new vfs::index_node{};
				node->parent = parent;
				node->type = vfs::index_node_type::Directory;
				node->entryName = name;
				if (parent)
				{
					size_t filepathSz = parent->filepath.len + node->entryName.len + (parent != vfs::g_root);
					char* filepath = (char*)memcpy(new char[filepathSz + 1], parent->filepath.str, parent->filepath.len);
					filepath[(parent != vfs::g_root) ? parent->filepath.len : 0] = '/';
					memcpy(&filepath[((parent != vfs::g_root) ? parent->filepath.len : 0) + 1], name, node->entryName.len);
					filepath[filepathSz] = 0;
					node->filepath = filepath;
					auto& children = (int)(parent->flags & vfs::index_node_flags::IsMountPoint) ? parent->data.mPoint->root : parent->children;
					children.lock.Lock();
					if (!children.head)
						children.head = node;
					if (children.tail)
						children.tail->next = node;
					node->prev = children.tail;
					children.tail = node;
					children.nNodes++;
					children.lock.Unlock();
				}
				else
				{
					char* filepath = (char*)memcpy(new char[node->entryName.len + 1], name, node->entryName.len);
					filepath[node->entryName.len] = 0;
					node->filepath = filepath;
				}
				return node;
			};
		vfs::g_root = setupDirectory("/", nullptr);
		vfs::index_node* root_foo = setupDirectory("foo", vfs::g_root);
		vfs::index_node* root_bar = setupDirectory("bar", vfs::g_root);
		vfs::index_node* root_foo_baz = setupDirectory("baz", root_foo);
		scheduler::ExitCurrentThread();
	}
}

#if UINT32_MAX == UINTPTR_MAX
#define STACK_CHK_GUARD 0xe2dee396
#else
#define STACK_CHK_GUARD 0x1C747501613CB3
#endif
 
extern "C" uintptr_t __stack_chk_guard;
uintptr_t __stack_chk_guard = STACK_CHK_GUARD;
 
extern "C" [[noreturn]] void __stack_chk_fail(void)
{
	obos::logger::panic(nullptr, "Stack corruption detected!\n");
}