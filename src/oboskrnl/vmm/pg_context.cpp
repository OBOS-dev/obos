/*
	oboskrnl/vmm/pg_context.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <todo.h>
#include <klog.h>

#include <arch/vmm_context.h>
#include <arch/vmm_defines.h>

#include <vmm/pg_context.h>
#include <vmm/page_node.h>
#include <vmm/init.h>

namespace obos
{
	namespace vmm
	{
		Context::Context() noexcept
		{ 
			m_owns = true;
			m_internalContext = new arch::pg_context{};
			m_internalContext->alloc();
		}
		void Context::AppendPageNode(const page_node& node)
		{
			page_node* newNode = (page_node*)g_pgNodeAllocator.Allocate(1);
			newNode->ctx = this;
			newNode->pageDescriptors = node.pageDescriptors;
			newNode->nPageDescriptors = node.nPageDescriptors;
			if (m_tail)
				m_tail->next = newNode;
			if(!m_head)
				m_head = newNode;
			newNode->prev = m_tail;
			m_tail = newNode;
			m_nNodes++;
		}
		void Context::RemovePageNode(void* virt)
		{
			page_node* node = ImplGetNode(virt);
			if (!node)
				return;
			if (node->next)
				node->next->prev = node->prev;
			if (node->prev)
				node->prev->next = node->next;
			if (m_head == node)
				m_head = node->next;
			if (m_tail == node)
				m_tail = node->prev;
			m_nNodes--;
			node->prev = nullptr;
			node->next = nullptr;
			g_pdAllocator.Free(node->pageDescriptors, node->nPageDescriptors);
			g_pgNodeAllocator.Free(node, 1);
		}
		page_node* Context::GetPageNode(void* addr) const
		{
			return ImplGetNode(addr);
		}

		Context::~Context() noexcept
		{
			if (m_owns)
			{
				m_internalContext->free();
				delete m_internalContext;
			}
			m_internalContext = nullptr;
			m_owns = false;
		}
		page_node* Context::ImplGetNode(void* virt) const
		{
			if (!OBOS_IS_VIRT_ADDR_CANONICAL(virt))
				return nullptr;
			for (page_node* cur = nullptr; cur;)
			{
				if (cur->pageDescriptors[0].virt >= (uintptr_t)virt && (uintptr_t)virt < (cur->pageDescriptors[cur->nPageDescriptors - 1].virt + (cur->pageDescriptors[cur->nPageDescriptors - 1].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)))
					return cur;

				cur = cur->next;
			}
			return nullptr;
		}
		
		int Context::Sort(bool ascendingOrder)
		{
			bool swapped = false;

			page_node* currentNode = m_head, *stepNode = nullptr;
			do
			{
				swapped = false;
				currentNode = m_head;
				if (!currentNode)
					break;

				while (currentNode->next != stepNode)
				{
					if (!currentNode)
						break;
					if (currentNode == currentNode->next)
						return -1;
					bool swap = ascendingOrder ?
						currentNode->pageDescriptors[0].virt > currentNode->next->pageDescriptors[0].virt : 
						currentNode->pageDescriptors[0].virt < currentNode->next->pageDescriptors[0].virt;
					if (swap)
					{
						swapNodes(currentNode, currentNode->next);
						swapped = true;
					}
					currentNode = currentNode->next;
					if (!currentNode)
						break;
				}
				stepNode = currentNode;
			} while (swapped);
			return 0;
		}
		void Context::swapNodes(page_node* node, page_node* with)
		{
			if (!node || !with)
				return;
			page_node* aPrev = node->prev;
			page_node* aNext = node->next;
			page_node* bPrev = with->prev;
			page_node* bNext = with->next;
			if (aPrev == with)
			{
				// Assuming the nodes are valid, bNext == node
				node->prev = bPrev;
				node->next = with;
				with->prev = node;
				with->next = aNext;
				if (bPrev) bPrev->next = node;
				if (aNext) aNext->prev = with;
			}
			else if (aNext == with)
			{
				// Assuming the nodes are valid, bPrev == node
				node->prev = with;
				node->next = bNext;
				with->prev = aPrev;
				with->next = node;
				if (bNext) bNext->prev = node;
				if (aPrev) aPrev->next = with;
			}
			else
			{
				node->prev = bPrev;
				node->next = bNext;
				with->prev = aPrev;
				with->next = aNext;
				if (aPrev) aPrev->next = with;
				if (aNext) aPrev->prev = with;
				if (bPrev) bPrev->next = node;
				if (bNext) bNext->prev = node;
			}
			if (m_head == with)
				m_head = node;
			else if (m_head == node)
				m_head = with;
			if (m_tail == with)
				m_tail = node;
			else if (m_tail == node)
				m_tail = with;
		}
	}
}