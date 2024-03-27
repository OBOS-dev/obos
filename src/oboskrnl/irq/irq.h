/*
	oboskrnl/irq/irq.h
 
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/irq_register.h>

namespace obos
{
	class Irq
	{
	public:
		Irq() = delete;
		Irq(uint8_t requiredIRQL, bool allowDefferedWorkSchedule = true);
		
		/// <summary>
		/// Sets the callback that checks whether this irq object is really the IRQ handler that should run.<br>
		/// This callback is required for the IRQ handler to run, and must be present.<br>
		/// For example, if this IRQ is for a PCI device, the callback might query the PCI registers whether this device is the one that interrupted the CPU.
		/// <summary>
		/// <param name="callback">The callback.</param>
		void SetIRQChecker(bool(*callback)(const Irq* irq, const struct IrqVector* vector, void* userdata), void* userdata);
		/// <summary>
		/// Sets the IRQ handler for this Irq object.
		/// <br>The passed frame parameter will be nullptr if the handler is called from a DPC.
		/// <summary>
		/// <param name="handler">The handler.</param>
		void SetHandler(void(*handler)(const Irq* irq, const IrqVector* vector, void* userdata, struct interrupt_frame* frame), void* userdata);
		auto GetHandler() const { return m_irqHandler.callback; };
		auto GetIRQChecker() const { return m_irqCheckerCallback.callback; };
		uint8_t GetVector() const;
		uint8_t GetIRQL() const;
	
		friend struct IrqVector;
		~Irq();
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