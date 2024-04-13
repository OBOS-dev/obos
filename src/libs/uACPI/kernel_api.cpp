/*
	libs/uACPI/kernel_api.cpp
	
	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <todo.h>
#include <klog.h>
#include <memmanip.h>

#include <uacpi/kernel_api.h>

#include <scheduler/scheduler.h>
#include <scheduler/cpu_local.h>
#include <scheduler/thread.h>

#include <uacpi_stdlib.h>

#include <irq/irq.h>
#include <irq/irql.h>

#include <vmm/init.h>
#include <vmm/map.h>

#include <allocators/basic_allocator.h>

#ifdef __x86_64__
#include <arch/x86_64/mm/palloc.h>
#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/hpet_table.h>
#endif

using namespace obos;

static bool isPower2(uint64_t num)
{
	auto firstBitPos = __builtin_ctzll(num);
	num &= ~(1<<firstBitPos);
	return num == 0 /* A power of two only ever has one bit set. */;
}

#if defined(__i386__) || defined(__x86_64__)
#	define spinlock_delay() asm volatile("pause")
#else
#	error Unknown arch.
#endif

extern "C"
{
	// uACPI kernel api.
	uacpi_status uacpi_kernel_raw_memory_read(uacpi_phys_addr address, uacpi_u8 byteWidth, uacpi_u64 *out_value)
	{
#ifdef __x86_64__
		void* virt = MapToHHDM(address);
#endif
		switch (byteWidth)
		{
			case 1: *out_value = *( uint8_t*)virt; break;
			case 2: *out_value = *(uint16_t*)virt; break;
			case 4: *out_value = *(uint32_t*)virt; break;
			case 8: *out_value = *(uint64_t*)virt; break;
			default: return UACPI_STATUS_INVALID_ARGUMENT;
		}
		return UACPI_STATUS_OK;
	}
	uacpi_status uacpi_kernel_raw_memory_write(uacpi_phys_addr address, uacpi_u8 byteWidth, uacpi_u64 in_value)
	{
#ifdef __x86_64__
		void* virt = MapToHHDM(address);
#endif
		switch (byteWidth)
		{
			case 1: *( uint8_t*)virt = in_value; break;
			case 2: *(uint16_t*)virt = in_value; break;
			case 4: *(uint32_t*)virt = in_value; break;
			case 8: *(uint64_t*)virt = in_value; break;
			default: return UACPI_STATUS_INVALID_ARGUMENT;
		}
		return UACPI_STATUS_OK;
	}
	uacpi_status uacpi_kernel_raw_io_read(uacpi_io_addr address, uacpi_u8 byteWidth, uacpi_u64 *out_value)
	{
		uint8_t (*readB)(uacpi_io_addr address) = nullptr;
		uint16_t(*readW)(uacpi_io_addr address) = nullptr;
		uint32_t(*readD)(uacpi_io_addr address) = nullptr;
		uint64_t(*readQ)(uacpi_io_addr address) = nullptr;
#ifdef __x86_64__
		readB = [](uacpi_io_addr address) -> uint8_t  { return inb(address); };
		readW = [](uacpi_io_addr address) -> uint16_t { return inw(address); };
		readD = [](uacpi_io_addr address) -> uint32_t { return ind(address); };
		readQ = nullptr; // Not supported.
#endif
		if (!isPower2(byteWidth))
			return UACPI_STATUS_INVALID_ARGUMENT;
		if (byteWidth > 8)
			return UACPI_STATUS_INVALID_ARGUMENT;
		uacpi_status status = UACPI_STATUS_OK;
		if (byteWidth == 1)
			if (readB)
				*out_value = readB(address);
			else
				status = UACPI_STATUS_INVALID_ARGUMENT;
		if (byteWidth == 2)
			if (readW)
				*out_value = readW(address);
			else
				status = UACPI_STATUS_INVALID_ARGUMENT;
		if (byteWidth == 4)
			if (readD)
				*out_value = readD(address);
			else
				status = UACPI_STATUS_INVALID_ARGUMENT;
		if (byteWidth == 8)
			if (readQ)
				*out_value = readQ(address);
			else
				status = UACPI_STATUS_INVALID_ARGUMENT;
		return status;
	}
	uacpi_status uacpi_kernel_raw_io_write(uacpi_io_addr address, uacpi_u8 byteWidth, uacpi_u64 in_value)
	{
		void(*writeB)(uacpi_io_addr address, uint8_t  val) = nullptr;
		void(*writeW)(uacpi_io_addr address, uint16_t val) = nullptr;
		void(*writeD)(uacpi_io_addr address, uint32_t val) = nullptr;
		void(*writeQ)(uacpi_io_addr address, uint64_t val) = nullptr;
#ifdef __x86_64__
		writeB = [](uacpi_io_addr address, uint8_t  val) -> void { outb(address, val); };
		writeW = [](uacpi_io_addr address, uint16_t val) -> void { outw(address, val); };
		writeD = [](uacpi_io_addr address, uint32_t val) -> void { outd(address, val); };
		writeQ = nullptr; // Not supported.
#endif
		if (!isPower2(byteWidth))
			return UACPI_STATUS_INVALID_ARGUMENT;
		if (byteWidth > 8)
			return UACPI_STATUS_INVALID_ARGUMENT;
		uacpi_status status = UACPI_STATUS_OK;
		if (byteWidth == 1)
			if (writeB)
				writeB(address, in_value);
			else
				status = UACPI_STATUS_INVALID_ARGUMENT;
		if (byteWidth == 2)
			if (writeW)
				writeW(address, in_value);
			else
				status = UACPI_STATUS_INVALID_ARGUMENT;
		if (byteWidth == 4)
			if (writeD)
				writeD(address, in_value);
			else
				status = UACPI_STATUS_INVALID_ARGUMENT;
		if (byteWidth == 8)
			if (writeQ)
				writeQ(address, in_value);
			else
				status = UACPI_STATUS_INVALID_ARGUMENT;
		return status;
	}
#ifdef __x86_64__
	void pciWriteByteRegister(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t data)
    {
        uint32_t address;
        uint32_t lbus = (uint32_t)bus;
        uint32_t lslot = (uint32_t)slot;
        uint32_t lfunc = (uint32_t)func;

        address = (uint32_t)((lbus << 16) | (lslot << 11) |
            (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

        outd(0xCF8, address);
        outb(0xCFC, data);
    }
    void pciWriteWordRegister(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t data)
    {
        uint32_t address;
        uint32_t lbus = (uint32_t)bus;
        uint32_t lslot = (uint32_t)slot;
        uint32_t lfunc = (uint32_t)func;

        address = (uint32_t)((lbus << 16) | (lslot << 11) |
            (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

        outd(0xCF8, address);
        outw(0xCFC, data);
    }
    void pciWriteDwordRegister(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t data)
    {
        uint32_t address;
        uint32_t lbus = (uint32_t)bus;
        uint32_t lslot = (uint32_t)slot;
        uint32_t lfunc = (uint32_t)func;

        address = (uint32_t)((lbus << 16) | (lslot << 11) |
            (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

        outd(0xCF8, address);
        outd(0xCFC, data);
    }
	 uint8_t pciReadByteRegister(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
        {
            uint32_t address;
            uint32_t lbus = (uint32_t)bus;
            uint32_t lslot = (uint32_t)slot;
            uint32_t lfunc = (uint32_t)func;

            address = (uint32_t)((lbus << 16) | (lslot << 11) |
                (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

            outd(0xCF8, address);

            uint8_t ret = (uint16_t)((ind(0xCFC) >> ((offset & 2) * 8)) & 0xFFFFFF);
            return ret;
        }
        uint16_t pciReadWordRegister(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
        {
            uint32_t address;
            uint32_t lbus = (uint32_t)bus;
            uint32_t lslot = (uint32_t)slot;
            uint32_t lfunc = (uint32_t)func;

            address = (uint32_t)((lbus << 16) | (lslot << 11) |
                (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

            outd(0xCF8, address);

            uint16_t ret = (uint16_t)((ind(0xCFC) >> ((offset & 2) * 8)) & 0xFFFF);
            return ret;
        }
        uint32_t pciReadDwordRegister(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
        {
            uint32_t address;
            uint32_t lbus = (uint32_t)bus;
            uint32_t lslot = (uint32_t)slot;
            uint32_t lfunc = (uint32_t)func;

            address = (uint32_t)((lbus << 16) | (lslot << 11) |
                (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));

            outd(0xCF8, address);

            return ((ind(0xCFC) >> ((offset & 2) * 8)));
        }
#endif
	uacpi_status uacpi_kernel_pci_read(
		uacpi_pci_address *address, uacpi_size offset,
		uacpi_u8 byte_width, uacpi_u64 *value
	)
	{
		if (address->segment)
			return UACPI_STATUS_UNIMPLEMENTED;
		switch (byte_width)
		{
			case 1: *value = pciReadByteRegister(address->bus, address->device, address->function, offset); break;
			case 2: *value = pciReadWordRegister(address->bus, address->device, address->function, offset); break;
			case 4: *value = pciReadDwordRegister(address->bus, address->device, address->function, offset); break;
			default: return UACPI_STATUS_INVALID_ARGUMENT;
		}
		return UACPI_STATUS_OK;
	}
	uacpi_status uacpi_kernel_pci_write(
		uacpi_pci_address *address, uacpi_size offset,
		uacpi_u8 byte_width, uacpi_u64 value
	)
	{
		if (address->segment)
			return UACPI_STATUS_UNIMPLEMENTED;
		switch (byte_width)
		{
			case 1: pciWriteByteRegister(address->bus, address->device, address->function, offset, value & 0xff); break;
			case 2: pciWriteWordRegister(address->bus, address->device, address->function, offset, value & 0xffff); break;
			case 4: pciWriteDwordRegister(address->bus, address->device, address->function, offset, value & 0xffff'ffff); break;
			default: return UACPI_STATUS_INVALID_ARGUMENT;
		}
		return UACPI_STATUS_OK;
	}
	// static allocators::BasicAllocator s_uACPIAllocator;
	// static bool s_uACPIAllocatorInitialized = false;
	void* uacpi_kernel_alloc(uacpi_size size)
	{
		static size_t nAllocationsSinceLastFree = 0;
		// logger::debug("Attempting allocation of %lu bytes.\n", size);
		// if (!s_uACPIAllocatorInitialized)
		// {
			// new (&s_uACPIAllocator) allocators::BasicAllocator{};
			// s_uACPIAllocatorInitialized = true;
		// }
		void* ret = new uint8_t[size];
		if (!ret)
			logger::warning("%s: Allocation of 0x%lx bytes failed.\n", __func__, size);
		/*else
			logger::debug("Allocated %lu bytes at 0x%p\n", size, ret);*/
		return ret;
	}
	void* uacpi_kernel_calloc(uacpi_size count, uacpi_size size)
	{
		return memzero(uacpi_kernel_alloc(count * size), count * size);
	}
	void uacpi_kernel_free(void* mem)
	{
		if (!mem)
			return;
		if (mem == (void*)0xffff8000feed50f0)
			asm volatile("nop");
		// logger::debug("Attempt free of 0x%p\n", mem);
		// if (!s_uACPIAllocatorInitialized)
			// logger::panic(nullptr, "Function %s, line %d: free before uACPI allocator is initialized detected. This is a bug, please report in some way.\n", 
			// __func__, __LINE__);
		size_t sz = allocators::g_kAllocator->QueryObjectSize(mem);
		if (sz == SIZE_MAX)
			logger::panic(nullptr, "Function %s, line %d: free of object by uACPI not allocated by uACPI allocator. This is a bug, please report in some way.\n", 
			__func__, __LINE__);
		delete[] (uint8_t*)mem;
		// s_uACPIAllocator.Free(mem, sz);
		//logger::debug("Freed 0x%p.\n", mem);
	}
	void uacpi_kernel_log(enum uacpi_log_level level, const char* format, ...)
	{
		va_list list;
		va_start(list, format);
		uacpi_kernel_vlog(level, format, list);
		va_end(list);
	}
	void uacpi_kernel_vlog(enum uacpi_log_level level, const char* format, uacpi_va_list list)
	{
		const char* prefix = "UNKNOWN";
		switch (level)
		{
		case UACPI_LOG_TRACE:
			prefix = "TRACE";
			break;
		case UACPI_LOG_INFO:
			prefix = "INFO";
			break;
		case UACPI_LOG_WARN:
			prefix = "WARN";
			break;
		case UACPI_LOG_ERROR:
			prefix = "ERROR";
			break;
		default:
			break;
		}
		logger::printf("uACPI, %s: ", prefix);
		logger::vprintf(format, list);
	}
	// 2500
	uacpi_u64 uacpi_kernel_get_ticks(void)
	{
		return scheduler::g_ticks * 2500; // 4000 hz -> 250000 ns = 2500 ticks.
	}
	void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size)
	{
#ifdef __x86_64__
		return MapToHHDM(addr);
#endif
	}
	void uacpi_kernel_unmap(void *, uacpi_size )
	{ /* Does nothing. */}
	uacpi_handle uacpi_kernel_create_spinlock(void)
	{
		return new bool{};
	}
	void uacpi_kernel_free_spinlock(uacpi_handle hnd)
	{
		delete (bool*)hnd;
	}
	uacpi_cpu_flags uacpi_kernel_spinlock_lock(uacpi_handle hnd)
	{
		bool* lock = (bool*)hnd;
		uacpi_cpu_flags irql = 0;
		RaiseIRQL(0xf, &irql);
		const bool excepted = false;
		while (__atomic_load_n(lock, __ATOMIC_SEQ_CST))
			spinlock_delay();
		while (__atomic_compare_exchange_n(lock, (bool*)&excepted, true, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			spinlock_delay();
		logger::debug("Locked %p.\n", hnd);
		return irql;
	}
	void uacpi_kernel_spinlock_unlock(uacpi_handle hnd, uacpi_cpu_flags oldIrql)
	{
		bool* lock = (bool*)hnd;
		__atomic_store_n(&lock, false, __ATOMIC_SEQ_CST);
		LowerIRQL(oldIrql);
	}
	uacpi_handle uacpi_kernel_create_event(void)
	{
		return new size_t{};
	}
	void uacpi_kernel_free_event(uacpi_handle e)
	{
		delete (size_t*)e;
	}

	uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle _e, uacpi_u16 t)
	{
		size_t* e = (size_t*)_e;
		if (t == 0xffff)
		{
			while (*e > 0);
			return UACPI_TRUE;
		}
		t *= 4;
		uint64_t wakeTime = scheduler::g_ticks + t;
		while (*e > 0 && scheduler::g_ticks >= wakeTime);
		bool ret = *e > 0;
		*e -= ret;
		return ret;
	}
	void uacpi_kernel_signal_event(uacpi_handle _e)
	{
		size_t* e = (size_t*)_e;
		__atomic_fetch_add(e, 1, __ATOMIC_SEQ_CST);
	}
	void uacpi_kernel_reset_event(uacpi_handle _e)
	{
		size_t* e = (size_t*)_e;
		__atomic_store_n(e, 0, __ATOMIC_SEQ_CST);
	}
	struct io_range
	{
		uacpi_io_addr base;
		uacpi_size len;
	};
	uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle)
	{
		if (base > 0xffff)
			return UACPI_STATUS_INVALID_ARGUMENT;
		io_range* rng = new io_range{ base, len };
		*out_handle = (uacpi_handle)rng;
		return UACPI_STATUS_OK;
	}
	void uacpi_kernel_io_unmap(uacpi_handle handle)
	{
		delete (io_range*)handle;
	}
	uacpi_status uacpi_kernel_io_read(
		uacpi_handle hnd, uacpi_size offset,
		uacpi_u8 byte_width, uacpi_u64 *value
	)
	{
		io_range* rng = (io_range*)hnd;
		if (offset >= rng->len)
			return UACPI_STATUS_INVALID_ARGUMENT;
		return uacpi_kernel_raw_io_read(rng->base + offset, byte_width, value);
	}
	uacpi_status uacpi_kernel_io_write(
		uacpi_handle hnd, uacpi_size offset,
		uacpi_u8 byte_width, uacpi_u64 value
	)
	{
		io_range* rng = (io_range*)hnd;
		if (offset >= rng->len)
			return UACPI_STATUS_INVALID_ARGUMENT;
		return uacpi_kernel_raw_io_write(rng->base + offset, byte_width, value);
	}
	struct mutex
	{
		bool locked = false;
		// If nullptr, locked.
		scheduler::Thread* owner = nullptr;
	};
	uacpi_handle uacpi_kernel_create_mutex(void)
	{
		return new mutex{};
	}
	void uacpi_kernel_free_mutex(uacpi_handle hnd)
	{
		delete (mutex*)hnd;
	}
	uacpi_bool uacpi_kernel_acquire_mutex(uacpi_handle hnd, uacpi_u16 t)
	{
		mutex *mut = (mutex*)hnd;
		uint64_t wakeTime = 0;
		if (t != 0xffff)
			wakeTime = scheduler::g_ticks + t * 4;
		else
			wakeTime = 0xffff'ffff'ffff'ffff;
		const bool expected = false;
		while (__atomic_load_n(&mut->locked, __ATOMIC_SEQ_CST))
			spinlock_delay();
		while (__atomic_compare_exchange_n(&mut->locked, (bool*)&expected, true, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) && scheduler::g_ticks < wakeTime)
			spinlock_delay();
		mut->owner = scheduler::GetCPUPtr()->currentThread;
		return UACPI_TRUE;
	}
	void uacpi_kernel_release_mutex(uacpi_handle hnd)
	{
		mutex *mut = (mutex*)hnd;
		if (mut->owner != scheduler::GetCPUPtr()->currentThread)
		{
			logger::debug("Failed release of mutex %p. Owner != currentThread\n", hnd);
			return;
		}
		mut->locked = false;
	}
	uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* req)
	{
		switch (req->type)
		{
		case UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT:
			break;
		case UACPI_FIRMWARE_REQUEST_TYPE_FATAL:
			logger::panic(nullptr,
				"Your bios fucked up, so now you have to deal with the consequences, also known as possible data loss. Firmware Error Code: 0x%016x, argument: %016lx\n",
				req->fatal.code, req->fatal.arg);
			break;
		default:
			break;
		}
		return UACPI_STATUS_OK;
	}
	#if defined(__x86_64__) || defined(_WIN64)
#pragma GCC push_options
#pragma GCC target("sse")
	uint64_t calibrateHPET(uint64_t freq);
	void uacpi_kernel_stall(uacpi_u8 usec)
	{
		// This (hopefully) should be enough context saved.
		uint8_t fpuState[512] = {};
		asm volatile("fxsave (%0)" : : "r"(fpuState):);
		for (uint64_t comparatorValue = calibrateHPET(1/(usec/1000000.f)); arch::g_hpetAddress->mainCounterValue < comparatorValue;)
			pause();
		asm volatile("fxrstor (%0)" : : "r"(fpuState):);
	}
	void uacpi_kernel_sleep(uacpi_u64 msec)
	{
		// This (hopefully) should be enough context saved.
		uint8_t fpuState[512] = {};
		asm volatile("fxsave (%0)" : : "r"(fpuState):);
		for (uint64_t comparatorValue = calibrateHPET(1.f/((float)msec)*1000.f); arch::g_hpetAddress->mainCounterValue < comparatorValue;)
			pause();
		asm volatile("fxrstor (%0)" : : "r"(fpuState):);
	}
#pragma GCC pop_options
#else
#error Implement uacpi_kernel_stall for the current architecture!
#endif
	uacpi_status uacpi_kernel_install_interrupt_handler(
		uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
		uacpi_handle *out_irq_handle
	)
	{
		Irq *irqHnd = new Irq{ irq, false, false };
		irqHnd->SetIRQChecker([](const Irq* , const struct IrqVector* , void* )->bool { return true; }, nullptr);
		uintptr_t *udata = new uintptr_t[2];
		udata[0] = (uintptr_t)ctx;
		udata[1] = (uintptr_t)handler;
		irqHnd->SetHandler([](const Irq*, const struct IrqVector*, void* udata, interrupt_frame*) {
			uacpi_handle ctx = *((void**)udata);
			uacpi_interrupt_handler handler = (uacpi_interrupt_handler)((void**)udata)[1];
			handler(ctx);
		}, udata);
		*out_irq_handle = irqHnd;
		return UACPI_STATUS_OK;
	}
	uacpi_status uacpi_kernel_uninstall_interrupt_handler(
		uacpi_interrupt_handler, uacpi_handle irq_handle
	)
	{
		Irq* irqHnd = (Irq*)irq_handle;
		delete[] (uintptr_t*)irqHnd->GetHandlerUserdata();
		delete irqHnd;
		return UACPI_STATUS_OK;
	}	
	struct uacpi_work
	{
		uacpi_work_type type;
		uacpi_work_handler cb; 
		uacpi_handle ctx;
		scheduler::Thread* dpc;
		
		uacpi_work *next, *prev;
	};
	static uacpi_work *s_workHead = nullptr, *s_workTail = nullptr;
	static size_t s_nWork = 0;
	static locks::SpinLock s_workQueueLock;
	static bool s_isWorkQueueLockInit = false;
	uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler cb, uacpi_handle ctx)
	{
		if (!s_isWorkQueueLockInit)
			new (&s_workQueueLock) locks::SpinLock{};
		// Make the work object.
		uacpi_work* work = new uacpi_work{ type, cb, ctx };
		s_workQueueLock.Lock();
		if(!s_workHead)
			s_workHead = work;
		if (s_workTail)
			s_workTail->next = work;
		work->prev = s_workTail;
		s_workTail = work;
		s_nWork++;
		s_workQueueLock.Unlock();
		// Make the DPC.
		using scheduler::Thread;
		scheduler::cpu_local *on = nullptr;
		if (type == UACPI_WORK_GPE_EXECUTION)
		{
			// Find the BSP in the cpu info list.
			for (size_t i = 0; i < scheduler::g_nCPUs && !on; i++)
				if (scheduler::g_cpuInfo[i].isBSP)
					on = &scheduler::g_cpuInfo[i];
		}
		else
			on = scheduler::GetCPUPtr();
		Thread* dpc = new Thread{};
		work->dpc = dpc;
		dpc->tid = scheduler::g_nextTID++;
		dpc->status = scheduler::ThreadStatus::CanRun;
		dpc->flags = scheduler::ThreadFlags::IsDeferredProcedureCall;
		dpc->priority = scheduler::ThreadPriority::High;
		dpc->affinity = dpc->ogAffinity = (1 << scheduler::GetCPUPtr()->cpuId);
		dpc->addressSpace = &vmm::g_kernelContext;
		arch::SetupThreadContext(&dpc->context, /* The thread's context */
								 &dpc->thread_stack,  /* The thread's stack info */
								 (uintptr_t)(void(*)(uacpi_work*))[](uacpi_work* work) { 
									// Run the work.
									work->cb(work->ctx);
									// Remove the work from the queue.
									s_workQueueLock.Lock();
									if (work->next)
										work->next->prev = work->prev;
									if (work->prev)
										work->prev->next = work->next;
									if (s_workTail == work)
										s_workTail = work->prev;
									if (s_workHead == work)
										s_workHead = work->next;
									s_nWork--;
									s_workQueueLock.Unlock();
									// Kill ourself.
									TODO("Do a more sane way of killing the current thread.");
									Thread *cur = scheduler::GetCPUPtr()->currentThread;
									cur->flags = (scheduler::ThreadFlags)((uint32_t)cur->flags | (uint32_t)scheduler::ThreadFlags::IsDead);
									scheduler::GetCPUPtr()->dpcList.Remove(cur);
									// vmm::Free(cur->addressSpace, (void*)cur->thread_stack.base, cur->thread_stack.size);
									if (!(--cur->referenceCount))
										delete cur;
									scheduler::GetCPUPtr()->currentThread = nullptr;
									scheduler::yield();
									while(1); 
								}, /* The thread's entry */
								 (uintptr_t)work, /* The thread's first parameter */
								 false, /* The thread's ring level */
								 0x8000, /* The thread's stack size */
								 dpc->addressSpace /* The thread's address space */
								);
		on->dpcList.Append(dpc);
		return UACPI_STATUS_OK;
	}
	uacpi_status uacpi_kernel_wait_for_work_completion(void)
	{
		while (s_nWork > 0)
			spinlock_delay();
		return UACPI_STATUS_OK;
	}
	

	// uacpi_stdlib
	void *uacpi_memcpy(void *dest, const void* src, size_t sz)
	{
		return memcpy(dest,src,sz);
	}
	void *uacpi_memset(void *dest, int src, size_t cnt)
	{
		return memset(dest, src, cnt);
	}
	int uacpi_memcmp(const void *src1, const void *src2, size_t cnt)
	{
		const uint8_t* b1 = (const uint8_t*)src1;
		const uint8_t* b2 = (const uint8_t*)src2;
		for (size_t i = 0; i < cnt; i++)
			if (b1[i] < b2[i])
				return -1;
			else if (b1[i] > b2[i])
				return 1;
			else
				continue;
		return 0;
	}
	int uacpi_strncmp(const char *src1, const char *src2, size_t maxcnt)
	{
		size_t len1 = uacpi_strnlen(src1, maxcnt);
		size_t len2 = uacpi_strnlen(src2, maxcnt);
		if (len1 < len2)
			return -1;
		else if (len1 > len2)
			return 1;
		return uacpi_memcmp(src1, src2, len1);
	}
	int uacpi_strcmp(const char *src1, const char *src2)
	{
		size_t len1 = uacpi_strlen(src1);
		size_t len2 = uacpi_strlen(src2);
		if (len1 < len2)
			return -1;
		else if (len1 > len2)
			return 1;
		return uacpi_memcmp(src1, src2, len1);
	}
	void *uacpi_memmove(void *dest, const void* src, size_t len)
	{
		if (src == dest)
			return dest;
		// Refactored from https://stackoverflow.com/a/65822606
		uint8_t *dp = (uint8_t *)dest;
		const uint8_t *sp = (uint8_t *)src;
		if(sp < dp && sp + len > dp)
		{
			sp += len;
			dp += len;
			while(len-- > 0)
				*--dp = *--sp;
		}
		else
			while(len-- > 0)
				*dp++ = *sp++;
		return dest;
	}
	size_t uacpi_strnlen(const char *src, size_t maxcnt)
	{
		size_t i = 0;
		for (; i < maxcnt && src[i]; i++);
		return i;
	}
	size_t uacpi_strlen(const char *src)
	{
		return strlen(src);
	}
	int uacpi_snprintf(char* dest, size_t n, const char* format, ...)
	{
		va_list list;
		va_start(list, format);
		int ret = logger::vsnprintf(dest, n, format, list);
		va_end(list);
		return ret;
	}
}