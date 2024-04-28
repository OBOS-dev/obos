/*
	oboskrnl/arch/x86_64/kdbg/kdbg.cpp
	
	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <console.h>

#include <arch/x86_64/kdbg/io.h>
#include <arch/x86_64/kdbg/bp.h>
#include <arch/x86_64/kdbg/debugger_state.h>
#include <arch/x86_64/kdbg/init.h>

#include <arch/x86_64/irq/interrupt_frame.h>
#include <arch/x86_64/irq/apic.h>

#include <arch/x86_64/hpet_table.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/smp_cpu_func.h>

#include <allocators/allocator.h>

#include <scheduler/cpu_local.h>

#include <vmm/init.h>
#include <vmm/mprot.h>

#include <irq/irq.h>
#include <irq/irql.h>

#include <elf/elf64.h>

#include <limine/limine.h>

#define TO_EHDR(obj) ((elf::Elf64_Ehdr*)obj)
#define TO_STRING_TABLE(ehdr, stable, off) ((const char*)ehdr + stable + off)

namespace obos
{
	static kdbg::cpu_local_debugger_state s_dbgState;
	extern volatile limine_kernel_file_request kernel_file;
	void addr2sym(uintptr_t rip, const char** const symbolName, uintptr_t* symbolBase, uint8_t symType);
	const elf::Elf64_Shdr* getSectionHeader(const elf::Elf64_Ehdr* ehdr, const char* name);
	namespace arch
	{
		extern Irq g_ipiIrq;
		bool LAPICTimerIRQChecker(const Irq*, const struct IrqVector*, void* _udata);
	}
	namespace scheduler
	{
		extern Irq g_schedulerIRQ;
	}
	namespace kdbg
	{
		debugger_state g_kdbgState;
		bool g_echoKernelLogsToDbgConsole = false;
#if OBOS_KDBG_ENABLED
		bp::bp(uintptr_t rip)
		{
			this->rip = rip;
			addr2sym(rip, &funcInfo.name, &funcInfo.base, elf::STT_FUNC);
		}
		void bp::setStatus(bool to)
		{
			if (enabled == to)
				return; // Nothing to do.
			enabled = to;
			// if (to)
				// g_kdbgState.activeBreakpoints.append(this);
			// else
				// g_kdbgState.activeBreakpoints.remove(this->idx);
		}
		bp::~bp()
		{
			// if (g_kdbgState.activeBreakpoints.contains(this->idx))
				// g_kdbgState.activeBreakpoints.remove(this->idx);
			// if (g_kdbgState.breakpoints.contains(this->idx))
				// g_kdbgState.breakpoints.remove(this->idx);
		}
		// bp_node* bp_list::getNode(size_t idx)
		// {
			// for (auto b = head; b; )
			// {
				// if (b->data->idx == idx)
					// return b;
				// b = b->next;
			// }
			// return nullptr;
		// }
		// void bp_list::append(bp* _breakpoint)
		// {
			// bp_node *breakpoint = new bp_node{};
			// breakpoint->data = _breakpoint;
			// if (tail)
				// tail->next = breakpoint;
			// if(!head)
				// head = breakpoint;
			// breakpoint->next = breakpoint;
			// tail = breakpoint;
			// _breakpoint->idx = nBreakpoints++;
		// }
		// void bp_list::remove(uint32_t idx)
		// {
			// bp_node *breakpoint = getNode(idx);
			// if (!breakpoint)
				// return;
			// if (breakpoint->next)
				// breakpoint->next->prev = breakpoint->prev;
			// if (breakpoint->prev)
				// breakpoint->prev->next = breakpoint->next;
			// if (breakpoint == head)
				// head = breakpoint->next;
			// if (breakpoint == tail)
				// tail = breakpoint->prev;
			// nBreakpoints--;
			// delete breakpoint;
		// }
		// bool bp_list::contains(size_t idx)
		// {
			// return getNode(idx) != nullptr;
		// }
		// bp& bp_list::operator[](size_t idx)
		// {
			// auto node = getNode(idx);
			// if (!node)
				// return *(bp*)nullptr;
			// return *node->data;
		// }
		static bool isWhitespace(char ch)
		{
			return
				ch == ' '  ||
				ch == '\t' ||
				ch == '\n' ||
				ch == '\v' ||
				ch == '\f' ||
				ch == '\r'; 
		}
		static const char *help_message = 
			"Usage:\n"
			"break [func]\n"
			"\tSets a breakpoint at func, or at the current position if no argument is specified.\n"
			"\tReturns the breakpoint number.\n"
			"break_at addr\n"
			"\tSets a breakpoint at addr.\n"
			"\tReturns the breakpoint number.\n"
			"delete breakpoint_idx\n"
			"\tDeletes the breakpoint with the index specified.\n"
			"\tReturns nothing on sucess.\n"
			"list\n"
			"\tLists breakpoints\n"
			"\tReturns all the active breakpoints.\n"
			"step\n"
			"\tSteps one instruction.\n"
			"\tReturns nothing.\n"
			"finish\n"
			"\tContinues until after the next ret instruction/\n"
			"\tReturns nothing.\n"
			"continue\n"
			"\tContinues until an exception or breakpoint occurs.\n"
			"\tReturns nothing.\n"
			"x/hex addr count\n"
			"\tPrints 'count' bytes at 'addr' as hexadecimal.\n"
			"\tReturns the bytes.\n"
			"x/dec addr count\n"
			"\tPrints 'count' bytes at 'addr' as decimal.\n"
			"\tReturns the bytes.\n"
			"x/i count [addr]\n"
			"\tDisassembles 'count' instructions at addr, or at rip if addr isn't specified.\n"
			"\tReturns the instructions disassembled.\n"
			"dreg\n"
			"\tDumps all the registers and their values.\n"
			"\tReturns the register's values and names.\n"
			"print register\n"
			"\tPrints a register as hexadecimal.\n"
			"\tReturns the register's value.\n"
			"set register=value\n"
			"\tSets the value of a register.\n"
			"\tReturns nothing.\n"
			"wb,ww,wd,wq address=value\n"
			"\tWrites a value in memory at a granularity of a byte, word, dword, or qword depending on the command overload.\n"
			"\tReturns nothing.\n"
			"where_addr\n"
			"\tConverts an address in the kernel to it's respective symbol.\n"
			"\tReturns the respective symbol name and address.\n"
			"where\n"
			"\tConverts a symbol to it's respective address.\n"
			"\tReturns the symbol name's address in hexadecimal.\n"
			// "watchdog seconds\n"
			// "\tSets a watchdog timer to go off after 'seconds' if no debug event happens. This implicitly continues.\n"
			// "\tReturns nothing.\n"
			"stack_trace\n"
			"\tPrints a stack trace.\n"
			"\tReturns the stack trace.\n"
			"ping\n"
			"\tPrints pong.\n"
			"\tReturns 'pong'\n"
			"echo on/off\n"
			"\tChanges whether kernel logs should be outputted on the debug console.\n"
			"\tReturns nothing\n"
			"echo ...\n"
			"\tEchoes a message onto the debug console.\n"
			"\tReturns the message\n"
			"echo\n"
			"\tPrints whether echoing kernel logs onto the debug console is enabled or not.\n"
			"\tReturns whether echoing kernel logs onto the debug console is enabled or not\n"
			"help\n"
			"\tPrints this help message.\n";
		bool step(cpu_local_debugger_state& dbg_state)
		{
			// Step one instruction.
			dbg_state.shouldStopAtNextInst = true;
			dbg_state.isFinishingFunction = false;
			dbg_state.context.frame.rflags |= RFLAGS_TRAP;
			return false;
		}
		bool cont(cpu_local_debugger_state& dbg_state)
		{
			dbg_state.shouldStopAtNextInst = false;
			dbg_state.isFinishingFunction = false;
			dbg_state.context.frame.rflags &= ~RFLAGS_TRAP;
			return false;
		}
		bool finish(cpu_local_debugger_state& dbg_state)
		{
			// Step one instruction.
			dbg_state.shouldStopAtNextInst = false;
			dbg_state.isFinishingFunction = true;
			dbg_state.nCallsSinceFinishCommand = 1;
			dbg_state.context.frame.rflags |= RFLAGS_TRAP;
			return false;
		}
		bool dreg(cpu_local_debugger_state& dbg_state)
		{
			auto frame = &dbg_state.context.frame;
			printf("Dumping registers:\n"
				   "\tRDI: 0x%016lx, RSI: 0x%016lx, RBP: 0x%016lx\n"
				   "\tRSP: 0x%016lx, RBX: 0x%016lx, RDX: 0x%016lx\n"
				   "\tRCX: 0x%016lx, RAX: 0x%016lx, RIP: 0x%016lx\n"
				   "\t R8: 0x%016lx,  R9: 0x%016lx, R10: 0x%016lx\n"
				   "\tR11: 0x%016lx, R12: 0x%016lx, R13: 0x%016lx\n"
				   "\tR14: 0x%016lx, R15: 0x%016lx, RFL: 0x%016lx\n"
				   "\t SS: 0x%016lx,  DS: 0x%016lx,  CS: 0x%016lx\n"
				   "\tCR0: 0x%016lx, CR2: 0x%016lx, CR3: 0x%016lx\n"
				   "\tCR4: 0x%016lx, CR8: 0x%016lx, EFER: 0x%016lx\n"
				   "\tGS_BASE: 0x%016lx, FS_BASE: 0x%016lx\n",
				   frame->rdi, frame->rsi, frame->rbp,
				   frame->rsp, frame->rbx, frame->rdx,
				   frame->rcx, frame->rax, frame->rip,
				   frame->r8, frame->r9, frame->r10,
				   frame->r11, frame->r12, frame->r13,
				   frame->r14, frame->r15, frame->rflags,
				   frame->ss, frame->ds, frame->cs,
				   getCR0(), getCR2(), getCR3(),
				   getCR4(), dbg_state.context.irql, getEFER(),
				   dbg_state.context.gs_base, dbg_state.context.fs_base
			);
			return true;
		}
		static bool isNumber(char ch)
		{
			char temp = ch - '0';
			return temp >= 0 && temp < 10;
		}
		static bool isHexNumber(char ch)
		{
			char temp = ch - '0';
			if (temp >= 0 && temp < 10)
				return true;
			temp = ch - 'A';
			if (temp >= 0 && temp < 6)
				return true;
			temp = ch - 'a';
			if (temp >= 0 && temp < 6)
				return true;
			return false;
		}
		static uint64_t dec2bin(const char* str, size_t size)
		{
			uint64_t ret = 0;
			for (size_t i = 0; i < size; i++)
			{
				if ((str[i] - '0') < 0 || (str[i] - '0') > 9)
					continue;
				ret *= 10;
				ret += str[i] - '0';
			}
			return ret;
		}
		static uint64_t hex2bin(const char* str, size_t size)
		{
			uint64_t ret = 0;
			str += *str == '\n';
			for (int i = size - 1, j = 0; i > -1; i--, j++)
			{
				char c = str[i];
				uintptr_t digit = 0;
				switch (c)
				{
				case '0':
				case '1':
				case '2':
				case '3':
				case '4':
				case '5':
				case '6':
				case '7':
				case '8':
				case '9':
					digit = c - '0';
					break;
				case 'A':
				case 'B':
				case 'C':
				case 'D':
				case 'E':
				case 'F':
					digit = (c - 'A') + 10;
					break;
				case 'a':
				case 'b':
				case 'c':
				case 'd':
				case 'e':
				case 'f':
					digit = (c - 'a') + 10;
					break;
				default:
					break;
				}
				ret |= digit << (j * 4);
			}
			return ret;
		}
		static uint64_t oct2bin(const char* str, size_t size)
		{
			uint64_t n = 0;
			const char* c = str;
			while (size-- > 0) 
				n = n * 8 + (uint64_t)(*c++ - '0');
			return n;
		}
		static uint64_t strtoull(const char* str, char** endptr, int base)
		{
			for (; !isNumber(*str) && *str; str++);
			if (!(*str))
				return UINT64_MAX;
			if (!base)
			{
				base = 10;
				if ((*(str + 1) == 'x' || *(str + 1) == 'X') && *str == '0')
				{
					base = 16;
					str += 2;
				}
				else if (*str == '0')
				{
					base = 8;
					str++;
				}
			}
			size_t sz = 0;
			for (; base == 16 ? isHexNumber(*str) : isNumber(*str) && *str; sz++, str++);
			str -= sz;
			if (!(*str))
				return UINT64_MAX;
			if (endptr)
				*endptr = (char*)str + sz;
			switch (base)
			{
			case 10:
				return dec2bin(str, sz);
			case 16:
				return hex2bin(str, sz);
			case 8:
				return oct2bin(str, sz);
			default:
				break;
			}
			return 0xffff'ffff'ffff'ffff;
		}
		bool examine_memory(bool asHexadecimal, void* at, size_t nBytes)
		{
			uint8_t* buf = (uint8_t*)at;
			vmm::page_descriptor pd;
			memzero(&pd, sizeof(pd) );
			printf("Dumping %lu bytes at address 0x%p as %s.\n", nBytes, at, asHexadecimal ? "hexadecimal" : "decimal");
			size_t lastPdCheckIndex = (size_t)-4096;
			size_t nBytesPrintedOnLine = 0;
			if (asHexadecimal)
				printf("00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n");
			else
				printf("  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15\n");
			for (size_t i = 0; i < nBytes; i++)
			{
				if ((lastPdCheckIndex + 4096) == i)
				{
					lastPdCheckIndex = i;
					OBOS_ASSERTP(vmm::GetPageDescriptor(&vmm::g_kernelContext, (void*)((uintptr_t)(buf + i) & ~0xfff), 4096, &pd, 1) != SIZE_MAX, "Could not retrieve page descriptors for page %p.",, buf);
				}
				if (!pd.present)
				{
					printf("%s ", asHexadecimal ? "??" : "???");
					continue;
				}
				if (nBytesPrintedOnLine++ == 16)
					printf("\n");
				if (!asHexadecimal)
					printf("%3d ", buf[i]);
				else
					printf("%02x ", buf[i]);
			}
			printf("\n");
			return true;
		}
		bool write_memory(void* at, uintptr_t what, uint8_t granularity)
		{
			vmm::page_descriptor pd[2];
			memzero(pd, sizeof(pd));
			OBOS_ASSERTP(vmm::GetPageDescriptor(&vmm::g_kernelContext, at, granularity, pd, 2) != SIZE_MAX, "Could not retrieve page descriptors for page %p.",, at);
			if (!pd[0].present)
			{
				printf("Cannot write %d bytes at 0x%p\n", granularity, at);
				return true;
			}
			if (OBOS_CROSSES_PAGE_BOUNDARY(at, granularity) && !pd[1].present)
			{
				printf("Cannot write %d bytes at 0x%p\n", granularity, at);
				return true;
			}
			switch(granularity)
			{
				case 1: *( uint8_t*)at = (what &        0xff); break;
				case 2: *(uint16_t*)at = (what &      0xffff); break;
				case 4: *(uint32_t*)at = (what & 0xffff'ffff); break;
				case 8: *(uint64_t*)at = what; break;
				default: return true;
			};
			return true;
		}
		uintptr_t& getRegister(cpu_local_debugger_state& dbg_state, const char* regName)
		{
			auto frame = &dbg_state.context.frame;
			uintptr_t *regVal = nullptr;
			if      (strcmp(regName, "rbp")) regVal = &frame->rbp;
			else if ( strcmp(regName, "r8")) regVal = &frame->r8;
			else if ( strcmp(regName, "r9")) regVal = &frame->r9;
			else if (strcmp(regName, "r10")) regVal = &frame->r10;
			else if (strcmp(regName, "r11")) regVal = &frame->r11;
			else if (strcmp(regName, "r12")) regVal = &frame->r12;
			else if (strcmp(regName, "r13")) regVal = &frame->r13;
			else if (strcmp(regName, "r14")) regVal = &frame->r14;
			else if (strcmp(regName, "r15")) regVal = &frame->r15;
			else if (strcmp(regName, "rdi")) regVal = &frame->rdi;
			else if (strcmp(regName, "rsi")) regVal = &frame->rsi;
			else if (strcmp(regName, "rbx")) regVal = &frame->rbx;
			else if (strcmp(regName, "rdx")) regVal = &frame->rdx;
			else if (strcmp(regName, "rcx")) regVal = &frame->rcx;
			else if (strcmp(regName, "rax")) regVal = &frame->rax;
			else if (strcmp(regName, "rip")) regVal = &frame->rip;
			else if (strcmp(regName, "rsp")) regVal = &frame->rsp;
			else if ( strcmp(regName, "ss")) regVal = &frame->ss;
			else if ( strcmp(regName, "ds")) regVal = &frame->ds;
			else if ( strcmp(regName, "cs")) regVal = &frame->cs;
			else if (strcmp(regName, "rflags")) regVal = &frame->rflags;
			else if (strcmp(regName, "irql") || strcmp(regName, "cr8")) regVal = &dbg_state.context.irql;
			else if (strcmp(regName, "cr3")) regVal = (uintptr_t*)dbg_state.context.pm;
			else if (strcmp(regName, "gs_base")) regVal = &dbg_state.context.gs_base;
			else if (strcmp(regName, "fs_base")) regVal = &dbg_state.context.fs_base;
			return *regVal;
		}
		bool print_reg(cpu_local_debugger_state& dbg_state, const char* regName)
		{
			uintptr_t tmp = 0;
			uintptr_t* regVal = &getRegister(dbg_state, regName);
			if (!regVal)
			{
				// Rather a special register or an invalid register.
				if (strcmp(regName, "cr0")) { tmp = getCR0(); }
				else if (strcmp(regName, "cr2")) { tmp = getCR2(); }
				else if (strcmp(regName, "cr4")) { tmp = getCR4(); }
				else
					return true;
				regVal = &tmp;
			}
			printf("%s=0x%016lx", regName, *regVal);
			if (strcmp(regName, "rip"))
			{
				const char* fName = nullptr;
				uintptr_t fBase = 0;
				addr2sym(*regVal, &fName, &fBase, elf::STT_FUNC);
				printf(" (%s+%ld)\n", fName ? fName : "External code", fBase ? *regVal-fBase : 0);
			}
			else
				printf("\n");
			return true;
		}
		bool write_reg(cpu_local_debugger_state& dbg_state, const char* regName, uintptr_t val)
		{
			uintptr_t* regVal = &getRegister(dbg_state, regName);
			if (!regVal)
			{
				// Rather a special register or an invalid register.
				if (strcmp(regName, "cr0")) asm volatile("mov %0, %%cr0;" : :"r"(val) :);
				else if (strcmp(regName, "cr2")) asm volatile("mov %0, %%cr2;" : :"r"(val) :);
				else if (strcmp(regName, "cr4")) asm volatile("mov %0, %%cr4;" : :"r"(val) :);
				return true;
			}
			*regVal = val;
			return true;
		}
		void set_drn_on_cpu(scheduler::cpu_local* cpu, uint8_t dbgIdx, uint64_t value)
		{
			// Setup the IPI.
			arch::ipi* drIPI = new arch::ipi{};
			arch::dbg_reg_ipi* payload = new arch::dbg_reg_ipi{};
			drIPI->data.dbg_reg = payload;
			drIPI->type = arch::ipi::IPI_DEBUG_REGISTER;
			payload->regIdx = dbgIdx;
			payload->val = new uint64_t{ value };
			payload->rw = true /* write */;
			// Add the IPI to the queue and call it.
			cpu->archSpecific.ipi_queue.push(drIPI);
			uint8_t oldIRQL = IRQL_INVALID;
			if (cpu == scheduler::GetCPUPtr() && GetIRQL() >= IRQL_IPI_DISPATCH)
			{
				// We need to temporarily lower the IRQL for self-ipis, as otherwise we would deadlock while waiting.
				oldIRQL = GetIRQL();
				LowerIRQL(IRQL_DISPATCH);
			}
			LAPIC_SendIPI(DestinationShorthand::None, DeliveryMode::Fixed, arch::g_ipiIrq.GetVector() + 0x20, cpu->cpuId);
			// Wait for the IPI to be processed.
			// We do this as we need to free the structures, and we don't want to cause a race condition.
			while(!drIPI->processed)
				pause();
			if (oldIRQL != IRQL_INVALID)			
			{
				uint8_t _oldIRQL = 0;
				RaiseIRQL(oldIRQL, &_oldIRQL);
			}
			// After the IPI has been processed, we must clean up after ourselves.
			// The IPI isn't in the queue anymore, so we just have to free the payload and ipi object.
			delete drIPI;
			delete payload->val;
			delete payload;
		}
		uint64_t get_drn_on_cpu(scheduler::cpu_local* cpu, uint8_t dbgIdx)
		{
			arch::ipi* drIPI = new arch::ipi{};
			arch::dbg_reg_ipi* payload = new arch::dbg_reg_ipi{};
			drIPI->data.dbg_reg = payload;
			drIPI->type = arch::ipi::IPI_DEBUG_REGISTER;
			payload->regIdx = dbgIdx;
			payload->val = new uint64_t{ 0 };
			payload->rw = false /* write */;
			// Add the IPI to the queue and call it.
			cpu->archSpecific.ipi_queue.push(drIPI);
			uint8_t oldIRQL = IRQL_INVALID;
			if (cpu == scheduler::GetCPUPtr() && GetIRQL() >= IRQL_IPI_DISPATCH)
			{
				// We need to temporarily lower the IRQL for self-ipis, as otherwise we would deadlock while waiting.
				oldIRQL = GetIRQL();
				LowerIRQL(IRQL_DISPATCH);
			}
			LAPIC_SendIPI(DestinationShorthand::None, DeliveryMode::Fixed, arch::g_ipiIrq.GetVector() + 0x20, cpu->cpuId);
			// Wait for the IPI to be processed.
			// We do this as we need to free the structures, and we don't want to cause a race condition.
			while(!drIPI->processed)
				pause();
			if (oldIRQL != IRQL_INVALID)			
			{
				uint8_t _oldIRQL = 0;
				RaiseIRQL(oldIRQL, &_oldIRQL);
			}
			// After the IPI has been processed, we must clean up after ourselves.
			// The IPI isn't in the queue anymore, so we just have to free the payload and ipi object.
			uint64_t val = *payload->val;
			delete drIPI;
			delete payload->val;
			delete payload;
			return val;
		}
		void setupDRsForBreakpoint(bp* _breakpoint)
		{
			switch (_breakpoint->idx)
			{
			case 0: asm volatile("mov %0, %%dr0" : : "r"(_breakpoint->rip) : ); break;
			case 1: asm volatile("mov %0, %%dr1" : : "r"(_breakpoint->rip) : ); break;
			case 2: asm volatile("mov %0, %%dr2" : : "r"(_breakpoint->rip) : ); break;
			case 3: asm volatile("mov %0, %%dr3" : : "r"(_breakpoint->rip) : ); break;
			default: break;
			}
			uintptr_t dr7 = 0;
			asm volatile("mov %%dr7, %0" :"=r"(dr7) ::);
			dr7 |= (1 << (_breakpoint->idx * 2 + 1));
			asm volatile("mov %0, %%dr7" ::"r"(dr7) : );
			for (size_t i = 0; i < scheduler::g_nCPUs; i++)
			{
				auto cpu = &scheduler::g_cpuInfo[i];
				if (cpu == scheduler::GetCPUPtr())
					continue;
				if (!cpu->initialized)
					continue;
				set_drn_on_cpu(cpu, _breakpoint->idx, _breakpoint->rip);
				dr7 = get_drn_on_cpu(cpu, 7);
				dr7 |= (1 << (_breakpoint->idx * 2 + 1));
				set_drn_on_cpu(cpu, 7, dr7);
			}
		}
		bool set_breakpoint(uintptr_t rip)
		{
			if (g_kdbgState.nBreakpointsInUse == 4)
			{
				printf("Breakpoint limit of four breakpoints has been hit.\n");
				return true;
			}
			auto _breakpoint = new bp{ rip };
			_breakpoint->idx = g_kdbgState.nextBpIndex;
			g_kdbgState.breakpoints[_breakpoint->idx] = _breakpoint;
			if (!g_kdbgState.breakpoints[g_kdbgState.nextBpIndex + 1])
				g_kdbgState.nextBpIndex++;
			else
			{
				g_kdbgState.nextBpIndex = 4;
				for (size_t i = 0; i < g_kdbgState.nBreakpointsInUse; i++)
				{	
					if (!g_kdbgState.breakpoints[i])
					{ 
						g_kdbgState.nextBpIndex = i; 
						break; 
					}
				}
			}
			// Set the appropriate debug registers.
			_breakpoint->awaitingSmpRefresh = arch::g_initializedAllCPUs;
			setupDRsForBreakpoint(_breakpoint);
			printf("Created breakpoint %d at rip=0x%p (%s+%ld).\n", _breakpoint->idx, (void*)rip, _breakpoint->funcInfo.name, rip-_breakpoint->funcInfo.base);
			return true;
		}
		bool delete_breakpoint(size_t idx)
		{
			if (idx > 4)
				return true;
			uintptr_t dr7 = 0;
			asm volatile("mov %%dr7, %0" :"=r"(dr7) ::);
			dr7 &= ~(1<<(idx*2+1));
			asm volatile("mov %0, %%dr7" ::"r"(dr7):);
			delete g_kdbgState.breakpoints[idx];
			g_kdbgState.breakpoints[idx] = nullptr;
			g_kdbgState.nextBpIndex = idx;
			return true;
		}
		bool list_breakpoints()
		{
			for (size_t i = 0; i < 4; i++)
			{
				if (!g_kdbgState.breakpoints[i])
					continue;
				printf("Breakpoint %d: 0x%p (%s+%ld)\nHit %d times\n",
					g_kdbgState.breakpoints[i]->idx, 
					(void*)g_kdbgState.breakpoints[i]->rip,
					g_kdbgState.breakpoints[i]->funcInfo.name, g_kdbgState.breakpoints[i]->rip-g_kdbgState.breakpoints[i]->funcInfo.base,
					g_kdbgState.breakpoints[i]->hitCount
				);
				// Example output:
				// Breakpoint 0: 0xffffffff80000000 (KernelArchInit+0)
				// Hit 0 times.
			}
			return true;
		}
		bool where_addr(uintptr_t addr)
		{
			const char *symName = nullptr;
			uintptr_t symBase = 0;
			addr2sym(addr, &symName, &symBase, elf::STT_NOTYPE);
			printf("0x%p: %s+%ld\n", (void*)addr, symName ? symName : "External code", symBase ? addr-symBase : 0);
			return true;
		}
		uintptr_t symToAddr(const char* symName, size_t* symSz, uint8_t symType)
		{
			uintptr_t base = (uintptr_t)kernel_file.response->kernel_file->address;
			elf::Elf64_Ehdr* ehdr = (elf::Elf64_Ehdr*)base;
			size_t stable = 0;
			if (!getSectionHeader(ehdr, ".strtab"))
				return UINTPTR_MAX;
			stable = getSectionHeader(ehdr, ".strtab")->sh_offset;
			const elf::Elf64_Shdr* symtab = getSectionHeader(ehdr, ".symtab");
			if (!symtab)
				return UINTPTR_MAX;
			size_t nEntries = symtab->sh_size / symtab->sh_entsize;
			for (size_t i = 0; i < nEntries; i++)
			{
				elf::Elf64_Sym* symbol = (elf::Elf64_Sym*)(base + symtab->sh_offset + i*symtab->sh_entsize);
				if (symType != elf::STT_NOTYPE)
					if ((symbol->st_info & 0xf) != symType)
						continue;
				if (strcmp(TO_STRING_TABLE(ehdr, stable, symbol->st_name), symName))
				{
					if (symSz)
						*symSz = symbol->st_size;
					return symbol->st_value;
				}
			}
			return UINTPTR_MAX;
		}
		bool where(const char* cmdline)
		{
			char* symName = nullptr;
			size_t symNameSz = 0;
			for(; !isWhitespace(cmdline[symNameSz]) && cmdline[symNameSz] != 0; symNameSz++);
			symName = new char[symNameSz + 1];
			memcpy(symName, cmdline, symNameSz);
			symName[symNameSz] = 0;
			size_t symSz = 0;
			uintptr_t addr = symToAddr(symName, &symSz, elf::STT_NOTYPE);
			delete[] symName;
			printf("%s is at 0x%p and ends at 0x%p.\n", symName, (void*)addr, (void*)(addr+symSz));
			return true;
		}
		bool _break(cpu_local_debugger_state& dbg_state, const char* cmdline)
		{
			if (!(*cmdline))
				return set_breakpoint(dbg_state.context.frame.rip);
			char* symName = nullptr;
			size_t symNameSz = 0;
			for(; !isWhitespace(cmdline[symNameSz]) && cmdline[symNameSz] != 0; symNameSz++);
			symName = new char[symNameSz + 1];
			memcpy(symName, cmdline, symNameSz);
			symName[symNameSz] = 0;
			size_t symSz = 0;
			uintptr_t addr = symToAddr(symName, &symSz, elf::STT_NOTYPE);
			delete[] symName;
			return set_breakpoint(addr);
		}
		bool stack_trace(cpu_local_debugger_state& dbg_state)
		{
			const char* fName = nullptr;
			uintptr_t fBase = 0;
			addr2sym(dbg_state.context.frame.rip, &fName, &fBase, elf::STT_FUNC);
			printf("Stack trace:\n");
			printf("\t0x%016lx: %s+%ld\n", dbg_state.context.frame.rip, fName ? fName : "External code", fBase ? dbg_state.context.frame.rip - fBase : 0);
			logger::stackTrace((void*)dbg_state.context.frame.rbp, "\t", printf);
			return true;
		}
		bool dbg_terminal();
		// TODO: Fix
		// void watchdog_irq_handler(const Irq*, const IrqVector*, void* udata, interrupt_frame* frame)
		// {
			// uint64_t *userdata = (uint64_t*)udata;
			// const uint64_t& nSeconds = userdata[0];
			// uint64_t& nSecondsPassed = userdata[1];
			// cpu_local_debugger_state& dbg_state = scheduler::GetCPUPtr()->archSpecific.debugger_state;
			// dbg_state.context.frame = *frame;
			// if (++nSecondsPassed >= nSeconds)
			// {
				// // The watchdog timer went off!
				// uint8_t oldIRQL = 0;
				// RaiseIRQL(0xf, &oldIRQL);
				// printf("Watchdog timer went off!\nCalling debug terminal...\n");
				// bool ret = dbg_terminal();
				// *frame = dbg_state.context.frame;
				// LowerIRQL(oldIRQL);
				// return;
			// }
		// }
		// bool set_watchdog(cpu_local_debugger_state& dbg_state, uint64_t nSeconds)
		// {
			// if (!g_kdbgState.watchdogIRQInitialized)
			// {
				// new (&g_kdbgState.watchdogIRQ) Irq{ scheduler::g_schedulerIRQ.GetVector(), false, false };
				// g_kdbgState.watchdogIRQInitialized = true;
			// }
			// constexpr uint64_t freqHz = 1;
			// if (g_kdbgState.watchdogIRQ.GetHandlerUserdata())
				// delete[] (uint64_t*)g_kdbgState.watchdogIRQ.GetHandlerUserdata();
			// if (g_kdbgState.watchdogIRQ.GetIrqCheckerUserdata())
				// delete[] (uint64_t*)g_kdbgState.watchdogIRQ.GetIrqCheckerUserdata();
			// auto udata = new uint64_t[3];
			// arch::g_hpetAddress->generalConfig &= ~(1<<0);
			// udata[0] = freqHz;
			// udata[1] = arch::g_hpetAddress->mainCounterValue;
			// udata[2] = udata[1] + (arch::g_hpetFrequency/freqHz);
			// arch::g_hpetAddress->generalConfig |= (1<<0);
			// g_kdbgState.watchdogIRQ.SetIRQChecker(arch::LAPICTimerIRQChecker, udata);
			// udata = new uint64_t[2];
			// udata[0] = nSeconds;
			// udata[1] = 0;
			// g_kdbgState.watchdogIRQ.SetHandler(watchdog_irq_handler, udata);
			// dbg_state.shouldStopAtNextInst = false;
			// dbg_state.isFinishingFunction = false;
			// return false;
		// }
		bool disasm(void* at, size_t nInstructions);
		struct colour_changer
		{
			colour_changer() = default;
			colour_changer(Pixel fore)
			{
				g_kernelConsole.GetColour(oldForeground, oldBackground);
				g_kernelConsole.SetColour(fore, oldBackground);
			}	
			~colour_changer()
			{
				g_kernelConsole.SetColour(oldForeground, oldBackground);
			}
			Pixel oldForeground, oldBackground;
		};
		struct unique_ptr
		{
			unique_ptr() = delete;
			unique_ptr(colour_changer* ptr)
				:obj{ptr}
			{}
			~unique_ptr() 
			{
				if (obj)
					delete obj;
			}
			colour_changer* obj;
		};
		bool dbg_terminal()
		{
			colour_changer *c;
			if (g_outputDev == output_format::CONSOLE)
				c = new colour_changer{ logger::GREY };
			else
				c = new colour_changer{};
			unique_ptr pC = c;
			cpu_local_debugger_state& dbg_state = scheduler::GetCPUPtr() ? scheduler::GetCPUPtr()->archSpecific.debugger_state : s_dbgState;
			bool shouldRun = true;
			while(shouldRun)
			{
				printf("> ");
				char* buf = getline();
				if (!buf)
				{
					putchar('\n');
					continue;
				}
				char* input = buf;
				for(; isWhitespace(*input); input++);
				size_t szInput = strlen(input);
				if (input[szInput - 1] == '\r')
					input[szInput - 1] = 0;
				char* command = input;
				if (!*command)
					continue;
				size_t commandSz = 0;
				for(; !isWhitespace(command[commandSz]) && command[commandSz] != 0; commandSz++);
				command = new char[commandSz + 1];
				memcpy(command, input, commandSz);
				command[commandSz] = 0;
				char* cmdline = input + commandSz;
				for(; isWhitespace(*cmdline) && *cmdline; cmdline++);
				if (strcmp(command, "help"))
					printf("%s", help_message);
				else if (strcmp(command, "ping"))
					printf("pong\n");
				else if (strcmp(command, "step"))
					shouldRun = step(dbg_state);
				else if (strcmp(command, "finish"))
					shouldRun = finish(dbg_state);
				else if (strcmp(command, "continue"))
					shouldRun = cont(dbg_state);
				else if (strcmp(command, "dreg"))
					shouldRun = dreg(dbg_state);
				else if (strcmp(command, "list"))
					shouldRun = list_breakpoints();
				else if (strcmp(command, "break"))
					shouldRun = _break(dbg_state, cmdline);
				else if (strcmp(command, "stack_trace"))
					shouldRun = stack_trace(dbg_state);
				else if (strcmp(command, "echo"))
				{
					if (!(*cmdline))
					{
						printf("Echo is %s\n", g_echoKernelLogsToDbgConsole ? "on" : "off");
						goto end;
					}
					if (strcmp(cmdline, "on"))
						g_echoKernelLogsToDbgConsole = true;
					else if (strcmp(cmdline, "off"))
						g_echoKernelLogsToDbgConsole = false;
					else
						printf("%s\n", cmdline);
				}
				else if (strcmp(command, "print"))
				{
					if (!(*cmdline))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}					
					shouldRun = print_reg(dbg_state, cmdline);
				}
				// else if (strcmp(command, "watchdog"))
				// {
					// if (!(*cmdline))
					// {
						// printf("Insufficient parameters to %s.\n", command);
						// goto end;
					// }					
					// shouldRun = set_watchdog(dbg_state, strtoull(cmdline, nullptr, 0));
				// }
				else if (strcmp(command, "break_at"))
				{
					if (!(*cmdline))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}

					shouldRun = set_breakpoint(strtoull(cmdline, nullptr, 0));
				}
				else if (strcmp(command, "where_addr"))
				{
					if (!(*cmdline))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					shouldRun = where_addr(strtoull(cmdline, nullptr, 0));
				}
				else if (strcmp(command, "delete"))
				{
					if (!(*cmdline))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}				
					shouldRun = delete_breakpoint(strtoull(cmdline, nullptr, 0));
				}
				else if (strcmp(command, "where"))
				{
					if (!(*cmdline))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					shouldRun = where(cmdline);
				}
				else if (strcmp(command, "x/hex"))
				{
					if (!(*cmdline))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					void* at = nullptr;
					size_t nBytes = 0;
					char* cmdline = input + commandSz + 1;
					char* endptr = nullptr;
					at = (void*)strtoull(cmdline, &endptr, 0);
					if (!(*endptr))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					endptr++;
					if (!(*endptr))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					nBytes = strtoull(endptr, nullptr, 0);
					shouldRun = examine_memory(true, at, nBytes);
				}
				else if (strcmp(command, "x/dec"))
				{
					if (!(*cmdline))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					void* at = nullptr;
					size_t nBytes = 0;
					char* cmdline = input + commandSz + 1;
					if (!(*cmdline))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					char* endptr = nullptr;
					at = (void*)strtoull(cmdline, &endptr, 0);
					if (!(*endptr))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					endptr++;
					if (!(*endptr))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					nBytes = strtoull(endptr, nullptr, 0);
					shouldRun = examine_memory(false, at, nBytes);
				}
				else if (strcmp(command, "x/i"))
				{
					if (!(*cmdline))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					void* at = nullptr;
					size_t nInstructions = 0;
					char* cmdline = input + commandSz + 1;
					if (!(*cmdline))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					char* endptr = nullptr;
					nInstructions = strtoull(cmdline, &endptr, 0);
					if (!(*endptr))
					{
						at = (void*)dbg_state.context.frame.rip;
						goto disassemble;
					}
					endptr++;
					at = (void*)strtoull(endptr, nullptr, 0);
					disassemble:
					shouldRun = disasm(at, nInstructions);
				}
				else if (strcmp(command, "set"))
				{
					if (!(*cmdline))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					size_t szPar1 = 0;
					for(; cmdline[szPar1] != '=' && cmdline[szPar1] != 0; szPar1++);
					if (!cmdline[szPar1])
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					uintptr_t val = strtoull(cmdline + szPar1 + 1, nullptr, 0);
					char* regName = (char*)memcpy(new char[szPar1 + 1], cmdline, szPar1);
					regName[szPar1] = 0;
					shouldRun = write_reg(dbg_state, regName, val);
					delete[] regName;
				}
				else if (strcmp(command, "wb") || 
						 strcmp(command, "ww") || 
						 strcmp(command, "wd") || 
						 strcmp(command, "wq")
						 )
				{
					uint8_t gran = 0;
					switch (command[1])
					{
						case 'b': gran = 1; break;
						case 'w': gran = 2; break;
						case 'd': gran = 4; break;
						case 'q': gran = 8; break;
						default: break;
					}
					if (!(*cmdline))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					void* at = (void*)strtoull(cmdline, &cmdline, 0);
					if (!(*cmdline))
					{
						printf("Insufficient parameters to %s.\n", command);
						goto end;
					}
					uintptr_t val = strtoull(cmdline, nullptr, 0);
					shouldRun = write_memory(at, val, gran);
				}
				else
					printf("Invalid command '%s'.\nUse 'help' for a list of valid commands.\n", command);
				end:
				delete[] command;
				allocators::g_kAllocator->Free(buf, allocators::g_kAllocator->QueryObjectSize(buf));
			}
			return false;
		}
		bool processDebugException(interrupt_frame* frame, bool isBpInstruction)
		{
			if (!g_initialized)
				return false; // We shouldn't be handling anything.
			cpu_local_debugger_state& dbg_state = scheduler::GetCPUPtr() ? scheduler::GetCPUPtr()->archSpecific.debugger_state : s_dbgState;
			dbg_state.context.frame = *frame;
			if (isBpInstruction)
			{
				const char* funcName = nullptr;
				uintptr_t funcBase = 0;
				addr2sym(frame->rip, &funcName, &funcBase, elf::STT_FUNC);
				printf("Trap instruction into kernel debugger at rip %p (%s+%d).\n", frame->rip, funcName ? funcName : "External Code", funcBase ? frame->rip-funcBase : funcBase);
				bool ret = dbg_terminal();
				*frame = dbg_state.context.frame;
				return ret;
			}
			uint64_t dr6 = getDR6();
			uint64_t dr6_bitmask = (1<<14)|(1<<0)|(1<<1)|(1<<2)|(1<<3);
			if (!(dr6 & dr6_bitmask))
				return true; // This debug exception isn't ours, as it didn't occur as a result of the trap flag or of any breakpoints.
			if (dr6 & (1<<14))
			{
				if (dbg_state.shouldStopAtNextInst)
				{
					dbg_state.shouldStopAtNextInst = false;
					printf("Opening debug terminal...\n");
					bool ret = dbg_terminal();
					*frame = dbg_state.context.frame;
					return ret;
				}
				if (dbg_state.isFinishingFunction)
				{
					// Our mini disassembler.
					constexpr uint8_t NEAR_RETURN1 = 0xc3;
					constexpr uint8_t NEAR_RETURN2 = 0xc2;
					constexpr uint8_t FAR_RETURN1 = 0xcb;
					constexpr uint8_t FAR_RETURN2 = 0xca;
					constexpr uint8_t CALL_REL = 0xe8;
					constexpr uint8_t CALL_ABS = 0xff;
					constexpr uint8_t CALL_FAR = 0x9a;
					constexpr uint8_t REX_W = 0x48;
					uint8_t instruction = *(uint8_t*)frame->rip;
					if (instruction == REX_W)
						instruction = *((uint8_t*)frame->rip + 1);
					switch(instruction)
					{
						case NEAR_RETURN1:
						case NEAR_RETURN2:
						case FAR_RETURN1:
						case FAR_RETURN2:
						{
							if (!(--dbg_state.nCallsSinceFinishCommand))
							{
								dbg_state.shouldStopAtNextInst = true;
								dbg_state.isFinishingFunction = false;
							}
							return false;
						}
						case CALL_REL:
						case CALL_ABS:
						case CALL_FAR:
						{
							dbg_state.nCallsSinceFinishCommand++;
							return false;
						}
					}
				}
			}
			else
			{
				// We hit a breakpoint...
				// Find the breakpoint(s) that were hit, and issue a message for each of them.
				// Then, call the debugger terminal.
				dr6 &= 0b01111;
				dr6 |= 0b10000;
				for (uint32_t idx = __builtin_ctzll(dr6); dr6 != 0b10000; )
				{
					if (!g_kdbgState.breakpoints[idx])
						goto end;
					{
						const char* funcName = nullptr;
						uintptr_t funcBase = 0;
						addr2sym(frame->rip, &funcName, &funcBase, elf::STT_FUNC);
						printf("Hit breakpoint %d at rip 0x%p (%s+%d).\n", idx, (void*)frame->rip, funcName ? funcName : "External Code", funcBase ? frame->rip-funcBase : funcBase);
						g_kdbgState.breakpoints[idx]->hitCount++;
					}
					end:
					dr6 &= ~(1<<idx);
					idx = __builtin_ctzll(dr6);
				}
				printf("Opening debug terminal...\n");
				bool ret = dbg_terminal();
				dbg_state.context.frame.rflags |= RFLAGS_RESUME;
				*frame = dbg_state.context.frame;
				return false;
			}
			return false;
		}
		bool exceptionHandler(interrupt_frame* frame)
		{
			if (!g_initialized)
				return true; // We shouldn't be handling anything.
			uint8_t oldIRQL = 0;
			RaiseIRQL(IRQL_MASK_ALL, &oldIRQL);
			cpu_local_debugger_state& dbg_state = scheduler::GetCPUPtr() ? scheduler::GetCPUPtr()->archSpecific.debugger_state : s_dbgState;
			dbg_state.context.frame = *frame;
			dbg_state.context.irql = oldIRQL;
			dbg_state.context.gs_base = rdmsr(0xC0000101);
			dbg_state.context.fs_base = rdmsr(0xC0000100);
			const char* exception_messages[32] = {
				"Division Error",
				"Debug",
				"Non-maskable Interrupt",
				"Breakpoint",
				"Overflow",
				"Bound Range Exceeded",
				"Invalid Opcode",
				"Device Not Available",
				"Double Fault",
				"Coprocessor Segment Overrun",
				"Invalid TSS",
				"Segment Not Present",
				"Stack-Segment Fault",
				"General Protection Fault",
				"Page Fault",
				"Reserved",
				"x87 Floating-Point Exception",
				"Alignment Check",
				"Machine Check",
				"SIMD Floating-Point Exception",
				"Virtualization Exception",
				"Control Protection Exception"
				"Reserved",
				"Reserved",
				"Reserved",
				"Reserved",
				"Reserved",
				"Hypervisor Injection Exception",
				"VMM Communication Exception",
				"Security Exception",
				"Reserved",
			};
			if (frame->intNumber == 3 || frame->intNumber == 1)
			{
				bool ret = processDebugException(frame, frame->intNumber == 3);
				LowerIRQL(oldIRQL);
				return ret;
			}
			const char* funcName = nullptr;
			uintptr_t funcBase = 0;
			addr2sym(frame->rip, &funcName, &funcBase, elf::STT_FUNC);
			printf("** EXCEPTION **\n"
				   "%s (%ld) exception occurred at rip 0x%p (%s+%d).\n"
				   "Opening debug terminal...\n", 
				   exception_messages[frame->intNumber], frame->intNumber, 
				   (void*)frame->rip, funcName ? funcName : "External Code", funcBase ? frame->rip-funcBase : funcBase);
			bool ret = dbg_terminal();
			*frame = dbg_state.context.frame;
			LowerIRQL(oldIRQL);
			return ret;
		}
#else
		bool exceptionHandler(interrupt_frame* )
		{ return false; }
#endif
	}
}