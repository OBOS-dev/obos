/*
	oboskrnl/scheduler/thread.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <scheduler/stack.h>

#include <arch/thr_context_info.h>

#include <vmm/pg_context.h>

namespace obos
{
	namespace scheduler
	{
		enum class ThreadStatus : uint32_t
		{
			Running,
			Blocked,
			Paused,
			CanRun,
		};
		enum class ThreadFlags : uint32_t
		{
			IsDead                  = 0x0001,
			IsDeferredProcedureCall = 0x0002,
		};
		enum class ThreadPriority : uint32_t
		{
			Idle,
			Low,
			Normal,
			High,
		};
		using ThrAffinity = __uint128_t;
		extern uint32_t g_nextTID;
		struct Thread
		{
			// Thread information
			uint32_t tid;
			uint32_t referenceCount;
			
			// Scheduler information
			ThreadStatus            status;
			ThreadFlags             flags;
			ThreadPriority          priority;
			ThrAffinity             affinity;
			ThrAffinity             ogAffinity;
			
			// Thread context
			thr_stack               thread_stack;
			arch::ThreadContextInfo context;
			vmm::Context*           addressSpace;
		};
		struct ThreadNode
		{
			ThreadNode *next, *prev;
			Thread* thr;
		};
		struct ThreadList
		{
			ThreadNode *head, *tail;
			size_t nNodes;
			void Append(Thread* thr);
			void Remove(Thread* thr);
			ThreadNode* Find(Thread* thr);
		};
	}
}