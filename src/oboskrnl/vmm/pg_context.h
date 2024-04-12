/*
	oboskrnl/vmm/pg_context.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/vmm_context.h>

#include <locks/spinlock.h>

namespace obos
{
	namespace vmm
	{
		class Context
		{
		public:
			Context() noexcept;
			Context(arch::pg_context* ctx) noexcept : m_internalContext{ ctx }, m_owns{ false } {}

			arch::pg_context* GetContext() const { return m_internalContext; }

			/// <summary>
			/// Appends a page node.
			/// </summary>
			/// <param name="node">The node to append. A copy of this is made and appended into the list.</param>
			void AppendPageNode(const struct page_node& node);
			/// <summary>
			/// Removes a page node from the list. This does not free the underlying descriptor.
			/// </summary>
			/// <param name="virt">The virtual address that the node represents.</param>
			void RemovePageNode(void* virt);
			/// <summary>
			/// Gets a page node based of an address.
			/// </summary>
			/// <param name="addr">The virtual address that the node represents.</param>
			/// <returns>The page node, or nullptr on failure.</returns>
			struct page_node* GetPageNode(void* addr) const;
			
			struct page_node* GetHead() const { return m_head; }
			struct page_node* GetTail() const { return m_tail; }
			
			bool Lock() { return m_lock.Lock(); }
			bool Unlock() { return m_lock.Unlock(); }
			bool Locked() const { return m_lock.Locked(); }

			~Context() noexcept;

			friend uintptr_t FindBase(Context* ctx, uintptr_t startRange, uintptr_t endRange, size_t size);
		private:
			struct page_node* ImplGetNode(void* virt) const;
			// -1: corruption.
			//  0: success.
			//  1: error
			int Sort(bool ascendingOrder = true);
			void swapNodes(struct page_node* node, struct page_node* with);
			arch::pg_context *m_internalContext = nullptr;
			bool m_owns = true;
			struct page_node *m_head, *m_tail;
			size_t m_nNodes;
			locks::SpinLock m_lock;
		};
	}
}