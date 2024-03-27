/*
	oboskrnl/arch/x86_64/cpu_local_arch.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

#include <arch/x86_64/mm/pmap_l4.h>

namespace obos
{
	class Irq;
	struct interrupt_frame;
	namespace arch
	{
		struct base_ipi
		{
			base_ipi() = default;
			base_ipi(void(*hnd)(base_ipi*, interrupt_frame*)) : handler {hnd} {};
			virtual ~base_ipi() {}
			void(*handler)(base_ipi* _this, interrupt_frame*);
		};
		// IPI to start the LAPIC timer.
		struct timer_ipi : public base_ipi
		{
		private:
			static void _handler(base_ipi* _this, interrupt_frame*);
		public:
			timer_ipi() : base_ipi{ _handler } {}
			const Irq* irq = nullptr;
			uint64_t freq = 0;
			bool singleShoot = false;
			virtual ~timer_ipi() {}
		};
		// TLB Shootdown
		struct shootdown_ipi : public base_ipi
		{
		private:
			static void _handler(base_ipi* _this, interrupt_frame*);
		public:
			shootdown_ipi() : base_ipi{ _handler } {}
			// If the current PageMap (cr3) is the same as the specified page map, then the address at 'virt' will 'invlpg'ed by the IPI handler.
			const PageMap* pm;
			uintptr_t virt;
			virtual ~shootdown_ipi() {}
		};
		struct ipi final
		{
			enum
			{
				IPI_INVALID = 0,
				IPI_TIMER,
				IPI_SHOOTDOWN,
			} type = IPI_INVALID;
			union
			{
				base_ipi* base;
				timer_ipi* timer;
				shootdown_ipi* tlb_shootdown;
			} data{};
			bool processed = false;
			ipi *next, *prev;
		};
		struct cpu_local_arch
		{
			cpu_local_arch()
			{
				gdt[0] = 0;
				gdt[1] = 0x00af9b000000ffff;
				gdt[2] = 0x00af93000000ffff;
				gdt[3] = 0x00aff3000000ffff;
				gdt[4] = 0x00affb000000ffff;
				gdt[5] = 0x0000000000000000;
				gdt[6] = 0x0000000000000000;
			}
			// Layout:
			// 0x00: Null descriptor.
			// 0x08: Kernel code descriptor.
			// 0x10: Kernel data descriptor.
			// 0x18: User data descriptor.
			// 0x20: User code descriptor.
			// 0x28: TSS descriptor.
			uint64_t gdt[7] = {
				0,
				0x00af9b000000ffff,
				0x00af93000000ffff,
				0x00aff3000000ffff,
				0x00affb000000ffff,
				// This is to be filled in the gdt initialize function of the SMP code.
				0x0000000000000000,
				0x0000000000000000,
			};
			struct
			{
				uint32_t rsv1;
				uint64_t rsp0;
				uint64_t rsp1;
				uint64_t rsp2;
				uint64_t rsv2;
				uint64_t ist0;
				uint64_t ist1;
				uint64_t ist2;
				uint64_t ist3;
				uint64_t ist4;
				uint64_t ist5;
				uint64_t ist6;
				uint64_t ist7;
				uint64_t rsv3;
				uint16_t rsv4;
				uint16_t iopb;
			} OBOS_PACK tss;
			struct
			{
				ipi *head, *tail;
				size_t nNodes;
				ipi *pop()
				{
					if (!head)
						return nullptr;
					ipi* ret = head;
					if (tail == ret)
						tail = ret->prev;
					if (ret->next)
						ret->next->prev = nullptr;
					head = head->next;
					nNodes--;
					return ret;
				}
				void push(ipi* i)
				{
					if (!head)
						head = i;
					if (tail)
						tail->next = i;
					i->prev = tail;
					tail = i;
					nNodes++;
				}
			} ipi_queue;
		};
	}
}