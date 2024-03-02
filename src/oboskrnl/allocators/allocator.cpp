/*
	oboskrnl/allocators/allocator.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <allocators/allocator.h>

namespace obos
{
	namespace allocators
	{
		Allocator* g_kAllocator;
		void* Allocator::ZeroAllocate(size_t size)
		{
			if (!size)
				return nullptr;
			size_t sizeBytes = GetAllocationSize(), nBytes = size;
			if (sizeBytes)
				nBytes *= sizeBytes;
			void* ptr = Allocate(size);
			if (!ptr)
				return nullptr;
			return memzero(ptr, nBytes);
		}
		Allocator::~Allocator()
		{
			OBOS_ASSERTP(g_kAllocator != this, "General kernel allocator object was destroyed. If this is expected, set g_kAllocator to something else, or nullptr for no default allocator (breaks all operator new/delete calls) before destruction.\n");
		}
	}
}
using obos::allocators::g_kAllocator;
void* operator new(size_t count) noexcept
{
	if (!g_kAllocator)
		return nullptr;
	if (!g_kAllocator->GetAllocationSize())
		return g_kAllocator->Allocate(count);
	const size_t allocSize = g_kAllocator->GetAllocationSize();
	count = (count / allocSize) + ((count % allocSize) != 0);
	return g_kAllocator->Allocate(count);
}
void* operator new[](size_t count) noexcept
{
	return operator new(count);
}
void operator delete (void* ptr) noexcept
{
	if (!g_kAllocator)
		return;
	const size_t count = g_kAllocator->QueryObjectSize(ptr);
	if (!count)
		return;
	g_kAllocator->Free(ptr, count);
}
void operator delete[](void* ptr) noexcept
{
	if (!g_kAllocator)
		return;
	const size_t size = g_kAllocator->QueryObjectSize(ptr);
	if (!size)
		return;
	operator delete(ptr, size);
}
void operator delete (void* ptr, size_t count) noexcept
{
	if (!g_kAllocator)
		return;
	const size_t allocSize = g_kAllocator->GetAllocationSize();
	count = (count / allocSize) + ((count % allocSize) != 0);
	g_kAllocator->Free(ptr, count);
}
void operator delete[](void* ptr, size_t size) noexcept
{
	if (!g_kAllocator)
		return;
	operator delete(ptr, size);
}

[[nodiscard]] void* operator new(size_t, void* ptr) noexcept { return ptr; }
[[nodiscard]] void* operator new[](size_t, void* ptr) noexcept { return ptr; }
void operator delete(void*, void*) noexcept {}
void operator delete[](void*, void*) noexcept {}