/*
	oboskrnl/arch/x86_64/cpu_local_arch.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <struct_packing.h>

#include <arch/x86_64/gdbstub/connection.h>

#include <irq/dpc.h>

typedef struct cpu_local_arch
{
	uint64_t gdtEntries[7];
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
	// Size: 0x20000 bytes, divided into the IST1 stack (offset 0 to 0x10000), and the cpu temp stack (offset 0x10000 to 0x20000)
	void* ist_stack;
	void* startup_stack; // Size: 0x4000 bytes, freed after smp initialization.
	bool initializedSchedulerTimer;
	bool pf_handler_running;
	gdb_ctx dbg_ctx;
	dpc dbg_dpc;
	uint64_t stack_check_guard;
} cpu_local_arch;
