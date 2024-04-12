/*
	oboskrnl/utils/vector.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <new>

#include <int.h>
#include <memmanip.h>
#ifdef OBOS_DEBUG
#include <klog.h>
#endif

#include <allocators/allocator.h>

namespace obos
{
	namespace utils
	{
		template <typename T>
		class Vector
		{
		public:
			Vector()
			{
				m_allocator = g_kernelAllocator;
			}
			Vector(allocators::Allocator* allocator)
				: m_allocator{ g_kernelAllocator }
			{}
			
			void push_back(const T& obj)
			{
				if (++m_sz > m_capacity)
					reserve(m_capacity + 4);
				new (&m_array + m_sz - 1) T{ obj };
			}
			void push_back(T&& obj)
			{
				if (++m_sz > m_capacity)
					reserve(m_capacity + 4);
				new (&m_array + m_sz - 1) T{ obj };
			}
			void pop_back()
			{
				auto& back = m_array[--m_sz];
				operator delete(m_array, &back);
				if (m_sz < (m_capacity - 4))
				{
					m_capacity -= 4;
					T* newArray = (T*)m_allocator.ReAllocate(m_array, byteSizeToAllocatorSize(m_capacity * sizeof(T)));
					if (newArray == (void*)UINTPTR_MAX)
					{
						// The allocator doesn't support ReAllocate.
						// Do it ourself.
						newArray = (T*)m_allocator.Allocate(byteSizeToAllocatorSize(m_capacity * sizeof(T)));
						memcpy(newArray, m_array, m_sz * sizeof(T));
						m_allocator.Free(m_array, m_allocator.QueryObjectSize(m_array));
					}
					if (!newArray)
						return;
					m_array = newArray;
				}
			}
			T& at(size_t i) const
			{
#ifdef OBOS_DEBUG
				OBOS_ASSERTP(i < m_sz, "Out of bounds vector access. Length: %lu. Index: %lu.\n",, m_sz, i);
#endif	
				return m_array[i];
			}
			
			void reserve(size_t capacity)
			{
				if (capacity <= m_capacity)
					return; // Nothing to do.
				m_capacity = capacity;
				T* newArray = (T*)m_allocator.ReAllocate(m_array, byteSizeToAllocatorSize(m_capacity * sizeof(T)));
				if (newArray == (void*)UINTPTR_MAX)
				{
					// The allocator doesn't support ReAllocate.
					// Do it ourself.
					newArray = (T*)m_allocator.Allocate(byteSizeToAllocatorSize(m_capacity * sizeof(T)));
					memcpy(newArray, m_array, m_sz * sizeof(T));
					m_allocator.Free(m_array, m_allocator.QueryObjectSize(m_array));
				}
				if (!newArray)
					return;
				m_array = newArray;
			}
			T* data() const { return m_array; }
			size_t length() const { return m_sz; }
			size_t capacity const { return m_capacity; }
		private:
			size_t byteSizeToAllocatorSize(size_t nBytes)
			{
				size_t blockSize = m_allocator.GetAllocationSize();
				if (!blockSize || blockSize == 1)
					return nBytes;
				return nBytes / blockSize + ((nBytes % blockSize) != 0);
			}
			allocators::Allocator* m_allocator;
			T *m_array;
			size_t m_len;
			size_t m_capacity;
		};
	}
}