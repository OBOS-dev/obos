/*
	oboskrnl/irq/irq.cpp
 
	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <klog.h>
#include <todo.h>

#include <irq/irq.h>

#include <arch/irq_register.h>

#include <arch/thr_context_info.h>

#include <allocators/slab.h>

#include <vmm/init.h>

#include <locks/spinlock.h>

#include <scheduler/thread.h>
#include <scheduler/cpu_local.h>
#include <scheduler/scheduler.h>
#include <scheduler/init.h>

namespace obos
{
	template <typename T>
	concept iframe = requires (T x) {
		{ x.vector };
	};
	IrqVectorList g_irqVectors;
	allocators::SlabAllocator g_irqVectorAllocator;
	static size_t s_irqlCapacities[14 /* The first two IRQLs are invalid for IrqVector.*/] = {
		8,8,8,8,8,
		8,8,8,8,8,
		8,8,8,8,
	};
	template<iframe T>
	void impl_verify_concept()
	{}
	void verify_concept()
	{
		impl_verify_concept<interrupt_frame>();
	}
	IrqVector* look_for_irql_in_list(uint8_t irql, size_t* nIrqVectorsForIRQL)
	{
		IrqVector *vector = nullptr;
		for (auto c = g_irqVectors.head; c;)
		{
			if ((c->vector >> 4) == irql)
			{
				if (vector->references.nNodes < s_irqlCapacities[irql - 2])
					vector = c;
				if (nIrqVectorsForIRQL)
					(*nIrqVectorsForIRQL)++;
				break;
			}
			c = c->next;
		}
		return vector;
	}
	Irq::Irq(uint8_t requiredIRQL, bool allowDefferedWorkSchedule)
		: m_allowDefferedWorkSchedule{ allowDefferedWorkSchedule }
	{		
		OBOS_ASSERTP(vmm::g_initialized, "Abstract IRQ interface cannot be used without the VMM initialized.");
		OBOS_ASSERTP(requiredIRQL >= 2, "IRQL for Irq must be less than 2, as IRQLs 0 and 1 are invalid in this case.");
		if (!g_irqVectorAllocator.GetAllocationSize())
		{
			// The allocator is uninitialized.
			new (&g_irqVectorAllocator) allocators::SlabAllocator{};
			g_irqVectorAllocator.Initialize(nullptr, sizeof(IrqVector));
		}
		// Look for a IrqVector node in the list that matches the required IRQL.
		size_t nIrqVectorsForIRQL = 0;
		IrqVector *vector = look_for_irql_in_list(requiredIRQL, &nIrqVectorsForIRQL);
		OBOS_ASSERTP(nIrqVectorsForIRQL <= OBOS_MAX_INTERRUPT_VECTORS_PER_IRQL, "Max interrupt vectors for IRQL %d has been exceeded. Report a bug if this issue persists.",, requiredIRQL);
		if (!vector)
		{
			if (nIrqVectorsForIRQL == OBOS_MAX_INTERRUPT_VECTORS_PER_IRQL)
			{
				// We need to re-use the IRQ vectors.
				s_irqlCapacities[requiredIRQL - 2] *= 2;
				vector = look_for_irql_in_list(requiredIRQL, nullptr);
			}
			else
			{
				// We should allocate a new IrqVector.
				vector = (IrqVector*)g_irqVectorAllocator.Allocate(1);
				vector->vector = OBOS_IRQL_TO_VECTOR(requiredIRQL) + nIrqVectorsForIRQL;
			}
		}
		vector->references.Append(this);
		vector->Register(IrqDispatcher);
		g_irqVectors.Append(vector);
		m_vector = vector;
	}
	uint8_t Irq::GetVector() const { return m_vector->vector; }
	uint8_t Irq::GetIRQL() const { return m_vector->vector >> 4; }
	void Irq::SetIRQChecker(bool(*callback)(const Irq* irq, const IrqVector* vector, void* userdata), void* userdata)
	{
		m_irqCheckerCallback.callback = callback;
		m_irqCheckerCallback.userdata = userdata;
	}
	void Irq::SetHandler(void(*handler)(const Irq* irq, const IrqVector* vector, void* userdata, interrupt_frame* frame), void* userdata)
	{
		m_irqHandler.callback = (decltype(m_irqHandler.callback))handler;
		m_irqHandler.userdata = userdata;
	}
	Irq::~Irq()
	{
		if (!m_vector)
			return;
		m_vector->references.Remove(this);
		if (!m_vector->references.nNodes)
			m_vector->Unregister();
	}
	void Irq::IrqDispatcher(interrupt_frame* frame)
	{
		IrqVector *vector = nullptr;
		for (auto c = g_irqVectors.head; c;)
		{
			if (c->vector == frame->vector)
			{
				vector = c;
				break;
			}
			
			c = c->next;
		}
		if (!vector)
		{
			logger::warning("%s: Could not find IrqVector object for interrupt vector %d.\n", __func__, frame->vector);
			return;
		}
		for (auto node = vector->references.head; node; )
		{
			Irq* cur = node->data;
			if (!cur->m_irqCheckerCallback.callback)
				continue;
			if (cur->m_irqCheckerCallback.callback(cur, vector, cur->m_irqCheckerCallback.userdata))
			{
				TODO("Run deffered work scheduling")
				if (!cur->m_allowDefferedWorkSchedule || !scheduler::g_initialized)
					((void(*)(const Irq* irq, const IrqVector* vector, void* userdata, interrupt_frame* frame))cur->m_irqHandler.callback)
						(cur, vector, cur->m_irqHandler.userdata, frame);
				else
				{
					scheduler::Thread* dpcObject = (scheduler::Thread*)scheduler::g_threadAllocator();
					dpcObject->tid = scheduler::g_nextTID++;
					dpcObject->status = scheduler::ThreadStatus::CanRun;
					dpcObject->flags = scheduler::ThreadFlags::IsDeferredProcedureCall;
					dpcObject->priority = scheduler::ThreadPriority::High;
					dpcObject->affinity = dpcObject->ogAffinity = (1 << scheduler::GetCPUPtr()->cpuId);
					dpcObject->addressSpace = &vmm::g_kernelContext;
					arch::SetupThreadContext(&dpcObject->context, /* The thread's context */
											 &dpcObject->thread_stack,  /* The thread's stack info */
											 (uintptr_t)(void(*)(Irq*))[](Irq* cur) { ((void(*)(const Irq* irq, const IrqVector* vector, void* userdata, interrupt_frame* frame))cur->m_irqHandler.callback)(cur, cur->m_vector, cur->m_irqHandler.userdata, nullptr); while(1); }, /* The thread's entry */
											 (uintptr_t)cur, /* The thread's first parameter */
											 false, /* The thread's ring level */
											 0x8000, /* The thread's stack size */
											 dpcObject->addressSpace /* The thread's address space */
											);
					scheduler::GetCPUPtr()->dpcList.Append(dpcObject);
				}
			}
		
			node = node->next;
		}
#if OBOS_NO_EOI_ON_SPURIOUS_INTERRUPT
		if (!arch::IsSpuriousInterrupt(frame))
#endif
		arch::SendEOI(frame);
	}
	void IrqVector::Register(void(*handler)(interrupt_frame* frame))
	{
		this->handler = handler;
		arch::RegisterInterrupt(vector, handler);
	}
	void IrqVector::Unregister()
	{
		if (handler)
			arch::RegisterInterrupt(vector, (handler = nullptr));
	}
	void IrqList::Append(Irq* obj) 
	{
		OBOS_ASSERTP(obj, "obj is nullptr.\n");
		IrqListNode* n = new IrqListNode{};
		n->data = obj;
		if (tail)
			tail->next = n;
		if(!head)
			head = n;
		n->prev = tail;
		tail = n;
		nNodes++;
	}
	void IrqList::Remove(Irq* obj) 
	{
		OBOS_ASSERTP(obj, "obj is nullptr.\n");
		IrqListNode* n = nullptr;
		for (auto iter = head; iter;)
		{
			if (iter->data == obj) 
			{
				n = iter;
				break;
			}
		
			iter = iter->next;
		}
		if (!n)
			return;
		if (n->prev)
			n->prev->next = n->next;
		if (n->next)
			n->next->prev = n->prev;
		if (head == n)
			head = n->next;
		if (tail == n)
			tail = n->prev;
		nNodes--;
		delete n;
	}
	void IrqVectorList::Append(IrqVector* node)
	{
		OBOS_ASSERTP(node, "node is nullptr.\n");
		if (tail)
			tail->next = node;
		if(!head)
			head = node;
		node->prev = tail;
		tail = node;
		nNodes++;
	}
	void IrqVectorList::Remove(IrqVector* node)
	{
		OBOS_ASSERTP(node, "node is nullptr.\n");
		IrqVector* n = nullptr;
		for (auto iter = head; iter;)
		{
			if (iter == node) 
			{
				n = iter;
				break;
			}
		
			iter = iter->next;
		}
		OBOS_ASSERTP(n, "node wasn't found in the current list.\n");
		n->prev->next = n->next;
		n->next->prev = n->prev;
		if (head == n)
			head = n->next;
		if (tail == n)
			tail = n->prev;
		n->prev = n->next = nullptr;
		nNodes--;
	}
}