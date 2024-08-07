/*
 * oboskrnl/irq/irq.h
 * 
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>

#include <irq/irql.h>

// Must define at least this:
// typedef impl_def_uint irq_vector_id;
// Note: Must have a member named 'vector' which contains the 'irq_vector_id' for the current interrupt
// typedef impl_def_struct interrupt_frame;
// The maximum value for irq_vector_id
// #define OBOS_IRQ_VECTOR_ID_MAX
// Converts an irq_vector_id to IRQL (for internal use only)
// #define OBOS_IRQ_VECTOR_ID_TO_IRQL(x)
// Converts an IRQL to an irq_vector_id (for internal use only)
// #define OBOS_IRQL_TO_IRQ_VECTOR_ID(x)
// The amount of 'irq_vector_id's per IRQL.
// #define OBOS_IRQ_VECTOR_ID_COUNT_PER_IRQL
#if defined(__x86_64__)
#	include <arch/x86_64/irq_vector.h>
#	include <arch/x86_64/interrupt_frame.h>
#	define OBOS_MAX_INTERRUPT_VECTORS (256-32)
#elif defined(__m68k__)
#	include <arch/m68k/interrupt_frame.h>
typedef uint8_t irq_vector_id;
#	define OBOS_MAX_INTERRUPT_VECTORS (256-64)
#	define OBOS_IRQ_VECTOR_ID_MAX (256)
#	define OBOS_IRQ_VECTOR_ID_COUNT_PER_IRQL (32)
#	define OBOS_IRQ_VECTOR_ID_TO_IRQL(x) ((irql)(((x)-64)/12+2))
#	define OBOS_IRQL_TO_IRQ_VECTOR_ID(x) ((irq_vector_id)(((x)<<4)))
#else
#	error Unknown platform.
#endif

#if !defined(OBOS_IRQ_VECTOR_ID_COUNT_PER_IRQL) || !defined(OBOS_IRQL_TO_IRQ_VECTOR_ID) || !defined(OBOS_IRQ_VECTOR_ID_TO_IRQL) || !defined(OBOS_IRQ_VECTOR_ID_MAX)
#	error Architecture platform includes are missing some macro defines.
#endif

typedef struct irq_node
{
	struct irq_node *next, *prev;
	struct irq* data;
} irq_node;
typedef struct irq_vector_node
{
	struct irq_vector_node*next, *prev;
	struct irq_vector* data;
} irq_vector_node;
typedef struct irq_list
{
	irq_node *head, *tail;
	size_t nNodes;
} irq_list;
typedef struct irq_vector_list
{
	irq_vector_node *head, *tail;
	size_t nNodes;
} irq_vector_list;
typedef struct irq_vector
{
	irq_vector_id id;
	irq_list irqObjects;
	size_t irqObjectsCapacity;
	bool allowWorkSharing;
	size_t nIRQsWithChosenID;
} irq_vector;
/// <summary>
/// Checks if this specific IRQ belongs to this IRQ object. Not called if vector->allowWorkSharing is false.
/// </summary>
/// <param name="i">The irq object.</param>
/// <param name="userdata">The user data.</param>
/// <returns>Whether this specific IRQ belongs to this IRQ object.</returns>
typedef bool(*check_irq_callback)(struct irq* i, void* userdata);
/// <summary>
/// Called when an IRQ is being moved from one vector to another. This is called before the move of the object.<para/>
/// Cannot reference any construction function in the abstract irq interface.<para/>
/// Note: Both vectors should always be of the same IRQL.
/// </summary>
/// <param name="i">The irq object.</param>
/// <param name="from">The old vector.</param>
/// <param name="to">The new vector.</param>
/// <param name="userdata">The user data.</param>
typedef void(*irq_move_callback)(struct irq* i, struct irq_vector* from, struct irq_vector* to, void* userdata);
/// <summary>
/// Handles an IRQ.
/// </summary>
/// <param name="i">The irq object.</param>
/// <param name="frame">The interrupt frame.</param>
/// <param name="userdata">The user data.</param>
/// <param name="oldIrql">The irql before the call of the irq dispatcher. Must be lowered before exit of the handler.</param>
typedef void(*irq_handler)(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql);
typedef struct irq
{
	irq_vector* vector;
	bool choseVector;
	void* irqCheckerUserdata;
	void* handlerUserdata;
	void* irqMoveCallbackUserdata;
	// Must be non-null if vector->allowWorkSharing is true.
	check_irq_callback irqChecker;
	irq_handler handler;
	irq_move_callback moveCallback;
} irq;

/// <summary>
/// Initializes the irq interface.
/// </summary>
/// <returns>The status of the function.</returns>
obos_status Core_InitializeIRQInterface();
/// <summary>
/// Allocates (but doesn't construct) an IRQ object.
/// </summary>
/// <param name="status">[out, optional] The status of the function.</param>
/// <returns>The object, or nullptr on failure.</returns>
OBOS_EXPORT irq* Core_IrqObjectAllocate(obos_status* status);
/// <summary>
/// Constructs an IRQ object.
/// </summary>
/// <paramref name="allowWorkSharing"/>
/// <param name="obj">The irq object.</param>
/// <param name="requiredIrql">The required irql of the vector.</param>
/// <param name="allowWorkSharing">Whether to allow work sharing of the vector. Shouldn't be used if it can be avoided, as it takes up more IRQ vectors, which could cause the system to run out.</param>
/// <param name="force">Whether to force work sharing to be disabled.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status Core_IrqObjectInitializeIRQL(irq* obj, irql requiredIrql, bool allowWorkSharing, bool force);
/// <summary>
/// Constructs an IRQ object.
/// </summary>
/// <paramref name="allowWorkSharing"/>
/// <paramref name="force"/>
/// <param name="obj">The irq object.</param>
/// <param name="vector">The required vector id.</param>
/// <param name="allowWorkSharing">See allowWorkSharing parameter for Core_IrqObjectInitializeIRQL</param>
/// <param name="force">See force parameter for Core_IrqObjectInitializeIRQL.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status Core_IrqObjectInitializeVector(irq* obj, irq_vector_id vector, bool allowWorkSharing, bool force);
/// <summary>
/// Frees and dereferences an IRQ object.<para/>
/// It is UB to use the irq object after.
/// </summary>
/// <param name="obj">The irq object.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status Core_IrqObjectFree(irq* obj);

/// <summary>
/// Registers an IRQ handler for the specified irq vector id.
/// </summary>
/// <param name="vector">The vector id.</param>
/// <param name="handler">The irq handler. If nullptr, the handler is cleared.</param>
/// <returns>The status of the function.</returns>
OBOS_WEAK obos_status CoreS_RegisterIRQHandler(irq_vector_id vector, void(*handler)(interrupt_frame* frame));
/// <summary>
/// Checks if an irq vector is in use.
/// </summary>
/// <param name="vector">The vector id.</param>
/// <returns>OBOS_STATUS_IN_USE if the vector is in use, OBOS_STATUS_SUCCESS if the vector is unused, any other status is an error code.</returns>
OBOS_WEAK obos_status CoreS_IsIRQVectorInUse(irq_vector_id vector);
/// <summary>
/// Sends an EOI to the IRQ controller.
/// </summary>
/// <param name="frame">The interrupt frame.</param>
OBOS_WEAK void CoreS_SendEOI(interrupt_frame* frame);
/// <summary>
/// Enters an IRQ handler.
/// </summary>
/// <param name="frame">The interrupt frame.</param>
/// <returns>Whether the IRQ dispatcher should continue running or not. This could be useful for e.g., IRQL emulation.</returns>
OBOS_WEAK bool CoreS_EnterIRQHandler(interrupt_frame* frame);
/// <summary>
/// Exits an IRQ handler.
/// </summary>
/// <param name="frame">The interrupt frame.</param>
OBOS_WEAK void CoreS_ExitIRQHandler(interrupt_frame* frame);