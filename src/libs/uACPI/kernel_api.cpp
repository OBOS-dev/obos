/*
	libs/uACPI/kernel_api.cpp
	
	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <uacpi/kernel_api.h>

#include <uacpi_stdlib.h>

#ifdef __x86_64__
#include <arch/x86_64/mm/palloc.h>
#include <arch/x86_64/asm_helpers.h>
#endif

using namespace obos;

static bool isPower2(uint64_t num)
{
	auto firstBitPos = __builtin_clzll(num);
	num &= ~(1<<firstBitPos);
	return num == 0 /* A power of two only ever has one bit set. */;
}

extern "C"
{
	// uACPI kernel api.
	uacpi_status uacpi_kernel_raw_memory_read(uacpi_phys_addr address, uacpi_u8 byte_width, uacpi_u64 *out_value)
	{
#ifdef __x86_64__
		void* virt = MapToHHDM(address);
#endif
		memcpy(out_value, virt, byte_width);
		return UACPI_STATUS_OK;
	}
	uacpi_status uacpi_kernel_raw_memory_write(uacpi_phys_addr address, uacpi_u8 byte_width, uacpi_u64 in_value)
	{
#ifdef __x86_64__
		void* virt = MapToHHDM(address);
#endif
		memcpy(virt, &in_value, byte_width);
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