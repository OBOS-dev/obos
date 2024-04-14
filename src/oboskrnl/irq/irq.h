/*
	oboskrnl/irq/irq.h
 
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <export.h>

#include <arch/irq_register.h>

namespace obos
{
	class Irq
	{
	public:
		Irq() = delete;
		OBOS_EXPORT Irq(uint8_t vec, bool allowDefferedWorkSchedule = true, bool isVecIRQL = true /* whether the 'vec' parameter is an IRQL level or a specific vector. */);
		
		/// <summary>
		/// Sets the callback that checks whether this irq object is really the IRQ handler that should run.<br>
		/// This callback is required for the IRQ handler to run, and must be present.<br>
		/// For example, if this IRQ is for a PCI device, the callback might query the PCI registers whether this device is the one that interrupted the CPU.
		/// <summary>
		/// <param name="callback">The callback.</param>
		OBOS_EXPORT void SetIRQChecker(bool(*callback)(const Irq* irq, const struct IrqVector* vector, void* userdata), void* userdata);
		/// <summary>
		/// Sets the IRQ handler for this Irq object.
		/// <br>The passed frame parameter will be nullptr if the handler is called from a DPC.
		/// <summary>
		/// <param name="handler">The handler.</param>
		OBOS_EXPORT void SetHandler(void(*handler)(const Irq* irq, const IrqVector* vector, void* userdata, struct interrupt_frame* frame), void* userdata);
		OBOS_EXPORT auto GetHandler() const { return m_irqHandler.callback; };
		OBOS_EXPORT auto GetIRQChecker() const { return m_irqCheckerCallback.callback; };
		OBOS_EXPORT uint8_t GetVector() const;
		OBOS_EXPORT uint8_t GetIRQL() const;
		
		OBOS_EXPORT void* GetIrqCheckerUserdata() const { return m_irqCheckerCallback.userdata; }
		OBOS_EXPORT void* GetHandlerUserdata() const { return m_irqHandler.userdata; }
	
		friend struct IrqVector;
		OBOS_EXPORT ~Irq();
	private:
		struct IrqVector *m_vector;
		struct cb
		{
			void* userdata;
			bool(*callback)(const Irq* irq, const IrqVector* vector, void* userdata);
		};
		cb m_irqCheckerCallback;
		cb m_irqHandler;
		bool m_allowDefferedWorkSchedule = true;
		static void IrqDispatcher(interrupt_frame* frame);
	};
	struct IrqListNode
	{
		IrqListNode *next, *prev;
		Irq* data;
	};
	struct IrqList
	{
		IrqListNode *head, *tail;
		size_t nNodes;
		void Append(Irq* obj);
		void Remove(Irq* obj);
	};
	struct IrqVector
	{
		void Register(void(*handler)(interrupt_frame* frame));
		void Unregister();
		
		uint8_t vector = 0;
		IrqList references{};
		
		IrqVector *next = nullptr, *prev = nullptr;
		
		void(*handler)(interrupt_frame*) = nullptr;
	};
	struct IrqVectorList
	{
		IrqVector *head = nullptr, *tail = nullptr;
		size_t nNodes = 0;
		void Append(IrqVector* node);
		void Remove(IrqVector* node);
	};
	extern IrqVectorList g_irqVectors;
}