/*
	oboskrnl/scheduler/thread.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <scheduler/thread.h>

namespace obos
{
	namespace scheduler
	{
		void ThreadList::Append(Thread* thr)
		{
			OBOS_ASSERTP(thr, "thr is nullptr.");
			ThreadNode* node = new ThreadNode{};
			node->thr = thr;
			if (tail)
				tail->next = node;
			if(!head)
				head = node;
			node->prev = tail;
			tail = node;
			nNodes++;
		}
		void ThreadList::Remove(Thread* thr)
		{
			ThreadNode* node = Find(thr);
			if (!node)
				return;
			if (node->next)
				node->next->prev = node->prev;
			if (node->prev)
				node->prev->next = node->next;
			if (head == node)
				head = node->next;
			if (tail == node)
				tail = node->prev;
			nNodes--;
			delete node;
		}
		ThreadNode* ThreadList::Find(Thread* thr)
		{
			for (auto c = head; c;)
			{
				if (thr == c->thr)
					return c;
				
				c = c->next;
			}
			return nullptr;
		}
	}
}