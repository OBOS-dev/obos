/*
 * oboskrnl/irq/irq.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <irq/irq.h>
#include <irq/irql.h>

#include <locks/spinlock.h>

#include <allocators/base.h>

irq* Core_IrqObjectAllocate(obos_status* status)
{
	return OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(irq), status);
}
static irq_vector s_irqVectors[OBOS_IRQ_VECTOR_ID_MAX];
static spinlock s_lock;
static bool s_irqInterfaceInitialized;
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
	irq_node* node = OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, sizeof(irq_node), nullptr);
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
static obos_status register_irq_vector(irq* obj, irq_vector_id id, bool allowWorkSharing, bool force)
{
	irq_vector* vector = &s_irqVectors[id];
	obj->vector = vector;
	if ((allowWorkSharing && vector->allowWorkSharing) || !vector->irqObjects.nNodes)
	{
		append_irq_to_vector(vector, obj);
		vector->allowWorkSharing = allowWorkSharing;
		return OBOS_STATUS_SUCCESS;
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
		return OBOS_STATUS_SUCCESS;
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
	return OBOS_STATUS_SUCCESS;
}
obos_status Core_IrqObjectInitializeIRQL(irq* obj, irql requiredIrql, bool allowWorkSharing, bool force)
{
	if (!s_irqInterfaceInitialized)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	if (!obj || requiredIrql > IRQL_MASKED || requiredIrql == 0 || requiredIrql == 1)
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