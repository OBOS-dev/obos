/*
 * oboskrnl/irq/irq.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <irq/irq.h>
#include <irq/irql.h>

#include <locks/spinlock.h>

#include <allocators/base.h>

#include <scheduler/cpu_local.h>

#include <mm/context.h>

irq* Core_IrqObjectAllocate(obos_status* status)
{
	if (!OBOS_NonPagedPoolAllocator)
		return ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(irq), status);
	return ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(irq), status);
}
static irq_vector s_irqVectors[OBOS_IRQ_VECTOR_ID_MAX];
static spinlock s_lock;
static bool s_irqInterfaceInitialized;
void Core_IRQDispatcher(interrupt_frame* frame)
{
#if !OBOS_ARCH_EMULATED_IRQL && !OBOS_LAZY_IRQL
	irql irql_ = OBOS_IRQ_VECTOR_ID_TO_IRQL(frame->vector);
	irql oldIrql2 = Core_RaiseIrqlNoThread(irql_);
	if (!CoreS_EnterIRQHandler(frame))
		return;
	CoreS_SendEOI(frame);
#elif OBOS_LAZY_IRQL
	irql irql_ = OBOS_IRQ_VECTOR_ID_TO_IRQL(frame->vector);
	if (irql_ < CoreS_GetCPULocalPtr()->currentIrql)
	{
		CoreS_SetIRQL(irql_, CoreS_GetIRQL());
		CoreS_DeferIRQ(frame);
		CoreS_SendEOI(frame);
		return;
	}
	if (!CoreS_EnterIRQHandler(frame))
		return;
	CoreS_SendEOI(frame);
	irql oldIrql2 = Core_RaiseIrqlNoThread(irql_);
#else
	irql irql_ = OBOS_IRQ_VECTOR_ID_TO_IRQL(frame->vector);
	if (!CoreS_EnterIRQHandler(frame))
		return; // some archs do IRQL emulation this way.
	if (irql_ <= Core_GetIrql())
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "IRQL on call of the dispatcher is less than the IRQL of the vector reported by the architecture (\"irql_ <= Core_GetIrql()\").");
	CoreS_SendEOI(frame);
	irql oldIrql2 = Core_RaiseIrqlNoThread(irql_);
#endif
	// irql oldIrql = Core_SpinlockAcquireExplicit(&s_lock, irql_, false);
	irq* irq_obj = nullptr;
	if (!s_irqVectors[frame->vector].allowWorkSharing)
	{
		irq_obj = s_irqVectors[frame->vector].irqObjects.head->data;
		// Core_SpinlockRelease(&s_lock, oldIrql);
	}
	else
	{
		irq_vector* vector = &s_irqVectors[frame->vector];
		for (irq_node* node = vector->irqObjects.head; node && !irq_obj; )
		{
			irq* cur = node->data;
			OBOS_ASSERT(cur->irqChecker); // to make sure the developer doesn't mess up; compiled out in release mode
			if (cur->irqChecker)
				if (cur->irqChecker(cur, cur->irqCheckerUserdata))
					irq_obj = cur;

			node = node->next;
		}
		// Core_SpinlockRelease(&s_lock, oldIrql);
	}
	if (!irq_obj)
	{
		//if (frame->vector == 0x60)
			//printf("could not resolve irq object\n");
		// Spooky actions from a distance...
		Core_LowerIrqlNoDPCDispatch(oldIrql2);
		CoreS_ExitIRQHandler(frame);
		return;
	}
	if (irq_obj->handler)
	{
		//if (frame->vector == 0x60)
			//printf("handling sci\n");
		irq_obj->handler(
			irq_obj,
			frame,
			irq_obj->handlerUserdata,
			oldIrql2);
	}
	CoreS_ExitIRQHandler(frame);
	Core_LowerIrqlNoThread(oldIrql2);
}
obos_status Core_InitializeIRQInterface()
{
	if (s_irqInterfaceInitialized)
		return OBOS_STATUS_ALREADY_INITIALIZED;
	for (irq_vector_id i = 0; i < sizeof(s_irqVectors) / sizeof(*s_irqVectors); i++)
	{
		s_irqVectors[i].allowWorkSharing = true;
		s_irqVectors[i].irqObjectsCapacity = 16;
		s_irqVectors[i].id = i;
		memzero(&s_irqVectors[i].irqObjects, sizeof(s_irqVectors[i].irqObjects));
	}
	s_irqInterfaceInitialized = true;
	return OBOS_STATUS_SUCCESS;
}
static void append_irq_to_vector(irq_vector* This, irq* what)
{
	OBOS_ASSERT(This);
	OBOS_ASSERT(what);
	irq_node* node = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(irq_node), nullptr);
	OBOS_ASSERT(node);
	node->data = what;
	if (!This->irqObjects.head)
		This->irqObjects.head = node;
	if (This->irqObjects.tail)
		This->irqObjects.tail->next = node;
	node->prev = This->irqObjects.tail;
	This->irqObjects.tail = node;
	This->irqObjects.nNodes++;
}
static void remove_irq_from_vector(irq_vector* This, irq_node* what)
{
	OBOS_ASSERT(This);
	OBOS_ASSERT(what);
	if (what->next)
		what->next->prev = what->prev;
	if (what->prev)
		what->prev->next = what->next;
	if (This->irqObjects.head == what)
		This->irqObjects.head = what->next;
	if (This->irqObjects.tail == what)
		This->irqObjects.tail = what->prev;
	This->irqObjects.nNodes--;
}
static obos_status register_irq_vector_handler(irq_vector_id id, void(*handler)(interrupt_frame*))
{
	obos_status s = OBOS_STATUS_SUCCESS;
	if (CoreS_IsIRQVectorInUse(id) == OBOS_STATUS_IN_USE)
		s = CoreS_RegisterIRQHandler(id, nullptr);
	if (s == OBOS_STATUS_SUCCESS)
		s = CoreS_RegisterIRQHandler(id, handler);
	return s;
}
static obos_status register_irq_vector(irq* obj, irq_vector_id id, bool allowWorkSharing, bool force)
{
	irq_vector* vector = &s_irqVectors[id];
	obj->vector = vector;
	if ((allowWorkSharing && vector->allowWorkSharing) || !vector->irqObjects.nNodes)
	{
		append_irq_to_vector(vector, obj);
		vector->allowWorkSharing = allowWorkSharing;
		return register_irq_vector_handler(vector->id, Core_IRQDispatcher);
	}
	if (!force)
		return OBOS_STATUS_IN_USE;
	if (allowWorkSharing && !vector->allowWorkSharing)
	{
		if (vector->irqObjects.head->data->choseVector)
			return OBOS_STATUS_IN_USE;
		// The other object didn't choose its own vector, so we can move it to another vector and put us on this vector.
		// Note: In this case, it is extremely likely that the vector id was chosen using Core_IrqObjectInitializeVector.
		// Anyway, just move this irq object to another empty vector of the same IRQL.
		// If another empty vector of the same IRQL does not exist, fail.
		uint8_t requiredIrql = OBOS_IRQ_VECTOR_ID_TO_IRQL(vector->id);
		irq_vector_id base = OBOS_IRQL_TO_IRQ_VECTOR_ID(requiredIrql);
		irq_vector_id found = OBOS_IRQ_VECTOR_ID_MAX;
		for (irq_vector_id vec = base; vec < OBOS_IRQ_VECTOR_ID_COUNT_PER_IRQL + base; vec++)
		{
			irq_vector* vector_ = &s_irqVectors[vec];
			OBOS_ASSERT(vector_->id == vec);
			OBOS_ASSERT(vector_->irqObjectsCapacity);
			if (!vector_->irqObjects.nNodes)
			{
				found = vec;
				break;
			}
		}
		if (found == OBOS_IRQ_VECTOR_ID_MAX)
			return OBOS_STATUS_IN_USE;
		// Move the irq object on the requested vector to another vector.
		irq_vector* newVector = &s_irqVectors[found];
		irq* cur = vector->irqObjects.head->data;
		if (cur->moveCallback)
			cur->moveCallback(cur, vector, newVector, cur->irqMoveCallbackUserdata);
		else
			OBOS_Warning("%s: IRQ Object 0x%p (IRQ Vector %d) does not have a move callback, and was moved.\n", __func__, cur, cur->vector->id);
		remove_irq_from_vector(vector, vector->irqObjects.head);
		append_irq_to_vector(newVector, cur);
		// Append this IRQ object to the vector.
		append_irq_to_vector(vector, obj);
		cur->vector = newVector;
		newVector->allowWorkSharing = vector->allowWorkSharing;
		vector->allowWorkSharing = allowWorkSharing;
		
		obos_status status = register_irq_vector_handler(vector->id, Core_IRQDispatcher);
		if (obos_is_error(status))
			return status;
		status = register_irq_vector_handler(newVector->id, Core_IRQDispatcher);
		return status;
	}
	// We've forced this.
	// Move the vectors in this vector to some other vector with the same IRQL.
	// Note: This should fail if one of the vectors set its own vector.
	if (vector->nIRQsWithChosenID)
		return OBOS_STATUS_IN_USE;
	uint8_t requiredIrql = OBOS_IRQ_VECTOR_ID_TO_IRQL(vector->id);
	irq_vector_id found = OBOS_IRQ_VECTOR_ID_MAX;
	irq_vector_id base = OBOS_IRQL_TO_IRQ_VECTOR_ID(requiredIrql);
	bool shouldIgnoreObjectCapacity = false;
find:
	for (irq_vector_id vec = base; vec < OBOS_IRQ_VECTOR_ID_COUNT_PER_IRQL + base; vec++)
	{
		irq_vector* vector_ = &s_irqVectors[vec];
		OBOS_ASSERT(vector_->id == vec);
		OBOS_ASSERT(vector_->irqObjectsCapacity);
		if (vector_ == vector)
			continue;
		if ((vector_->irqObjects.nNodes < vector_->irqObjectsCapacity || shouldIgnoreObjectCapacity) && (!vector_->nIRQsWithChosenID || (vector_->allowWorkSharing && allowWorkSharing)))
		{
			if (vector_->irqObjects.nNodes == 0)
				goto l1;
			if (vector_->allowWorkSharing && allowWorkSharing)
				goto l1;
			if (force)
				goto l1;
		l1:
			found = vec;
			if (shouldIgnoreObjectCapacity)
				vector_->irqObjectsCapacity += vector_->irqObjectsCapacity / 4; // Same as multiplying by 1.25
			break;
		}
	}
	if (found == OBOS_IRQ_VECTOR_ID_MAX)
	{
		if (!shouldIgnoreObjectCapacity)
			goto find;
		return OBOS_STATUS_NOT_FOUND;
	}
	irq_vector* newVector = &s_irqVectors[found];
	for (irq_node* node = vector->irqObjects.head; node; )
	{
		irq* cur = node->data;
		OBOS_ASSERT(cur);
		if (cur->moveCallback)
			cur->moveCallback(cur, vector, newVector, cur->irqMoveCallbackUserdata);
		else
			OBOS_Warning("%s: IRQ Object 0x%p (IRQ Vector %d) does not have a move callback, and was moved.\n", __func__, cur, cur->vector->id);
		irq_node* nnode = node->next;
		remove_irq_from_vector(vector, node);
		node = nnode;
		append_irq_to_vector(newVector, cur);
		cur->vector = newVector;
	}
	newVector->allowWorkSharing = vector->allowWorkSharing;
	vector->allowWorkSharing = false;
	append_irq_to_vector(vector, obj);
	obos_status status = register_irq_vector_handler(vector->id, Core_IRQDispatcher);
	if (obos_is_error(status))
		return status;
	status = register_irq_vector_handler(newVector->id, Core_IRQDispatcher);
	return status;
}
obos_status Core_IrqObjectInitializeIRQL(irq* obj, irql requiredIrql, bool allowWorkSharing, bool force)
{
	if (!s_irqInterfaceInitialized)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	if (!obj || requiredIrql > IRQL_MASKED || requiredIrql == 0
#if OBOS_IRQL_COUNT == 16
		|| requiredIrql == 1
#endif
	)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (obj->vector)
		return OBOS_STATUS_ALREADY_INITIALIZED;
	irql oldIrql = Core_SpinlockAcquire(&s_lock);
	irq_vector_id found = OBOS_IRQ_VECTOR_ID_MAX;
	irq_vector_id base = OBOS_IRQL_TO_IRQ_VECTOR_ID(requiredIrql);
	bool shouldIgnoreObjectCapacity = false;
	find:
	for (irq_vector_id vec = base; vec < OBOS_IRQ_VECTOR_ID_COUNT_PER_IRQL + base; vec++)
	{
		irq_vector* vector = &s_irqVectors[vec];
		OBOS_ASSERT(vector->id == vec);
		OBOS_ASSERT(vector->irqObjectsCapacity);
		if ((vector->irqObjects.nNodes < vector->irqObjectsCapacity || shouldIgnoreObjectCapacity) && (!vector->nIRQsWithChosenID || (vector->allowWorkSharing && allowWorkSharing)))
		{
			if (vector->irqObjects.nNodes == 0)
				goto l1;
			if (vector->allowWorkSharing && allowWorkSharing)
				goto l1;
			if (force)
				goto l1;
			l1:
			found = vec;
			if (shouldIgnoreObjectCapacity)
				vector->irqObjectsCapacity += vector->irqObjectsCapacity / 4; // Same as multiplying by 1.25
			break;
		}
	}
	if (found == OBOS_IRQ_VECTOR_ID_MAX)
	{
		if (!shouldIgnoreObjectCapacity)
			goto find;
		Core_SpinlockRelease(&s_lock, oldIrql);
		return OBOS_STATUS_NOT_FOUND;
	}
	obj->choseVector = false;
	obos_status res = register_irq_vector(obj, found, allowWorkSharing, force);
	Core_SpinlockRelease(&s_lock, oldIrql);
	return res;
}
obos_status Core_IrqObjectInitializeVector(irq* obj, irq_vector_id vector, bool allowWorkSharing, bool force)
{
	if (!s_irqInterfaceInitialized)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	if (!obj || vector >= OBOS_IRQ_VECTOR_ID_MAX)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (obj->vector)
		return OBOS_STATUS_ALREADY_INITIALIZED;
	obj->choseVector = true;
	irql oldIrql = Core_SpinlockAcquire(&s_lock);
	obos_status res = register_irq_vector(obj, vector, allowWorkSharing, force);
	obj->vector->nIRQsWithChosenID++;
	Core_SpinlockRelease(&s_lock, oldIrql);
	return res;
}
obos_status Core_IrqObjectFree(irq* obj)
{
	if (!s_irqInterfaceInitialized)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	if (!obj)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (obj->vector)
	{
		irql oldIrql = Core_SpinlockAcquire(&s_lock);
		irq_node* node = nullptr;
		if (!obj->vector->allowWorkSharing)
			node = obj->vector->irqObjects.head;
		else
		{
			for (irq_node* cur = obj->vector->irqObjects.head; cur && !node; )
			{
				if (cur->data == obj)
					node = cur;
				cur = cur->next;
			}
		}
		OBOS_ASSERT(node);
		OBOS_ASSERT(node->data == obj);
		remove_irq_from_vector(obj->vector, node);
		if (!obj->vector->irqObjects.nNodes)
		{
			obj->vector->allowWorkSharing = true;
			obj->vector->irqObjectsCapacity = 16;
		}
		obj->vector->nIRQsWithChosenID -= (size_t)obj->choseVector;
		Core_SpinlockRelease(&s_lock, oldIrql);
	}
	// FIXME: Set a free callback in the irq object instead of assuming the kernel allocator.
	Free(OBOS_KernelAllocator, obj, sizeof(*obj));
	return OBOS_STATUS_SUCCESS;
}
bool Core_IrqInterfaceInitialized()
{
	return s_irqInterfaceInitialized;
}
