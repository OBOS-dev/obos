/*
	oboskrnl/arch/x86_64/trace.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <todo.h>
#include <memmanip.h>

#include <vmm/page_descriptor.h>
#include <vmm/mprot.h>
#include <vmm/init.h>
#include <vmm/pg_context.h>

#include <scheduler/cpu_local.h>

#include <limine/limine.h>

#include <elf/elf64.h>

namespace obos
{
	extern volatile limine_kernel_file_request kernel_file;
	extern volatile limine_kernel_address_request kernel_addr;
#define TO_EHDR(obj) ((elf::Elf64_Ehdr*)obj)
#define TO_STRING_TABLE(ehdr, stable, off) ((const char*)ehdr + stable + off)
	const elf::Elf64_Shdr* getSectionHeader(const elf::Elf64_Ehdr* ehdr, const char* name)
	{
		uintptr_t base = (uintptr_t)ehdr;
		const elf::Elf64_Shdr* sections = (elf::Elf64_Shdr*)(base + ehdr->e_shoff);
		size_t shdr_stable = sections[ehdr->e_shstrndx].sh_offset;
		for (size_t i = 0; i < ehdr->e_shnum; i++)
			if (strcmp(TO_STRING_TABLE(ehdr, shdr_stable, sections[i].sh_name), name))
				return &sections[i];
		return nullptr;
	}
	void addr2sym(uintptr_t rip, const char** const symbolName, uintptr_t* symbolBase, uint8_t symType)
	{
		if (rip < kernel_addr.response->virtual_base)
		{
			*symbolName = nullptr;
			*symbolBase = 0;
			return;
		}
		uintptr_t base = (uintptr_t)kernel_file.response->kernel_file->address;
		elf::Elf64_Ehdr* ehdr = (elf::Elf64_Ehdr*)base;
		size_t stable = 0;
		if (getSectionHeader(ehdr, ".strtab"))
			stable = getSectionHeader(ehdr, ".strtab")->sh_offset;
		const elf::Elf64_Shdr* symtab = getSectionHeader(ehdr, ".symtab");
		if (!symtab)
		{
			*symbolName = nullptr;
			*symbolBase = 0;
			return;
		}
		size_t nEntries = symtab->sh_size / symtab->sh_entsize;
		for (size_t i = 0; i < nEntries; i++)
		{
			elf::Elf64_Sym* symbol = (elf::Elf64_Sym*)(base + symtab->sh_offset + i*symtab->sh_entsize);
			if (symType != elf::STT_NOTYPE)
				if ((symbol->st_info & 0xf) != symType)
					continue;
			if (rip >= symbol->st_value && rip < (symbol->st_value + symbol->st_size))
			{
				if (stable)
					*symbolName = TO_STRING_TABLE(ehdr, stable, symbol->st_name);
				else
					*symbolName = "no strtab";
				*symbolBase = symbol->st_value;
				return;
			}
		}
	}
	namespace logger
	{
		struct stack_frame
		{
			stack_frame* down;
			uintptr_t rip;
		};
		static void stackTraceNoFuncName(stack_frame* frame, const char* prefix)
		{
			while (frame)
			{
				printf("\t0x%016lx: Cannot get function name\n", frame->rip);
				frame = frame->down;
			}
		}
		void stackTrace(void* parameter, const char* prefix)
		{
			stack_frame* frame = parameter ? (stack_frame*)parameter : (stack_frame*)__builtin_frame_address(0);
			if (!vmm::g_initialized)
			{
				stackTraceNoFuncName(frame, prefix);
				return;
			}
			int32_t nFrames = 0;
			vmm::page_descriptor pd[2];
			memzero(pd, sizeof(pd));
			vmm::Context* ctx = &vmm::g_kernelContext;
			if (scheduler::GetCPUPtr() && scheduler::GetCPUPtr()->currentThread)
				ctx = scheduler::GetCPUPtr()->currentThread->addressSpace;
			do 
			{
				OBOS_ASSERTP(vmm::GetPageDescriptor(ctx, frame, sizeof(*frame), pd, 2) != SIZE_MAX, "Could not retrieve page descriptors for page %p.",, frame);
				for (size_t i = 0; i < (1 + (size_t)OBOS_CROSSES_PAGE_BOUNDARY(frame, sizeof *frame)); i++)
				{
					if (!pd[i].present)
					{
						frame = nullptr;
						if (nFrames)
							nFrames--;
						break;
					}
				}
				
				if (!frame)
					break;
				frame = frame->down;
				nFrames++;
			} while (frame);
			frame = parameter ? (stack_frame*)parameter : (stack_frame*)__builtin_frame_address(0);
			while (nFrames-- > 0)
			{
				const char* functionName = nullptr;
				uintptr_t functionStart = 0;
				addr2sym(frame->rip, &functionName, &functionStart, elf::STT_FUNC);
				if (functionName)
					printf("%s0x%016lx: %s+%ld\n", prefix, frame->rip, functionName, (uintptr_t)frame->rip - functionStart);
				else
					printf("%s0x%016lx: External Code\n", prefix, frame->rip);
				frame = frame->down;
			}
		}
	}
}