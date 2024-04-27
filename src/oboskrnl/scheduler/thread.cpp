/*
	oboskrnl/scheduler/thread.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <scheduler/thread.h>
#include <scheduler/cpu_local.h>
#include <scheduler/scheduler.h>

#include <arch/thr_context_info.h>

#include <vmm/map.h>

namespace obos
{
	namespace scheduler
	{
		template <typename T>
		concept tCtxInfo = requires (T x) {
			{ x.irql };
		};
		template<tCtxInfo T>
		void impl_verify_concept()
		{}
		void verify_concept()
		{
			impl_verify_concept<arch::ThreadContextInfo>();
		}

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
		static void ExitCurrentThreadImpl(uintptr_t)
		{
			if (!scheduler::GetCPUPtr())
				return;
			Thread* cur = scheduler::GetCPUPtr()->currentThread;
			if (!cur)
				return;
			cur->flags = (scheduler::ThreadFlags)((uint32_t)cur->flags | (uint32_t)scheduler::ThreadFlags::IsDead);
			if ((uint32_t)cur->flags & (uint32_t)scheduler::ThreadFlags::IsDeferredProcedureCall)
				scheduler::GetCPUPtr()->dpcList.Remove(cur);
			vmm::Free(cur->addressSpace, (void*)cur->thread_stack.base, cur->thread_stack.size);
			scheduler::GetCPUPtr()->currentThread = nullptr;
			if (!(--cur->referenceCount))
				delete cur;
			scheduler::yield();
		}
		[[noreturn]] void ExitCurrentThread()
		{
			arch::JumpToFunctionWithCPUTempStack(ExitCurrentThreadImpl, 0);
			while (1)
				scheduler::yield();
		}
		uint32_t GetCurrentTid()
		{
			if (!scheduler::GetCPUPtr())
				return 0xffffffff;
			Thread* cur = scheduler::GetCPUPtr()->currentThread;
			if (!cur)
				return 0xffffffff;
			return GetCPUPtr()->currentThread->tid;
		}
	}
}