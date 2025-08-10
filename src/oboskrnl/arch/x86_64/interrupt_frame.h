/*
	oboskrnl/arch/x86_64/interrupt_frame.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

enum
{
	RFLAGS_CARRY = BIT(0),
	RFLAGS_PARITY = BIT(2),
	RFLAGS_AUXILLARY_CARRY = BIT(4),
	RFLAGS_ZERO = BIT(6),
	RFLAGS_SIGN = BIT(7),
	RFLAGS_TRAP = BIT(8),
	RFLAGS_INTERRUPT_ENABLE = BIT(9),
	RFLAGS_DIRECTION = BIT(10),
	RFLAGS_OVERFLOW = BIT(11),
	RFLAGS_IOPL_3 = BIT(12) | BIT(13),
	RFLAGS_NESTED_TASK = BIT(14),
	RFLAGS_RESUME = BIT(16),
	RFLAGS_VIRTUAL8086 = BIT(17),
	RFLAGS_ALIGN_CHECK = BIT(18),
	RFLAGS_VINTERRUPT_FLAG = BIT(19),
	RFLAGS_VINTERRUPT_PENDING = BIT(20),
	RFLAGS_CPUID = BIT(21),
};
typedef struct __interrupt_frame
{
	// 0x0
	uintptr_t cr3;
	// 0x8
	uintptr_t ds;
	// 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58
	// 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90
	uintptr_t rbp, ignored1, r15, r14, r13, r12, r11, r10, r9, r8,
	rdi, rsi, ignored2, rbx, rdx, rcx, rax;
	// 0x98, 0xA0, 0xA8
	uintptr_t vector, intNumber, errorCode;
	// 0xB0, 0xB8
	uintptr_t rip, cs;
	// 0xC0
	uintptr_t rflags;
	// 0xC8, 0xD0
	uintptr_t rsp, ss;
} interrupt_frame;

typedef struct syscall_frame
{
	// ds=0x00, ss=0x1b, cs=0x23
	uintptr_t orig_rax;
	uintptr_t rbp, rip, r15, r14, r13, r12, r11;
	union {
		// the syscall trap handler saves user rsp in r10
		uintptr_t rsp;
		uintptr_t r10;
	};
	uintptr_t r9, r8, rdi, rsi, rbx, rdx, rcx, cr3;
} syscall_frame;