/*
 * oboskrnl/arch/m68k/memmanip.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>

OBOS_NO_KASAN OBOS_NO_UBSAN void* memset(void* blk, uint32_t val, size_t count)
{
	char* buf = (char*)blk;
	for (size_t i = 0; i < count; i++)
		buf[i] = val;
	return blk;
}
OBOS_NO_KASAN OBOS_NO_UBSAN void* memzero(void* blk, size_t count)
{
	return memset(blk, 0, count);
}
OBOS_NO_KASAN OBOS_NO_UBSAN void* memcpy(void* blk1_, const void* blk2_, size_t count)
{
	char *blk1 = (char*)blk1_;
	const char *blk2 = (char*)blk2_;
	for (size_t i = 0; i < count; i++)
		blk1[i] = blk2[i];
	return blk1_;
}
OBOS_NO_KASAN OBOS_NO_UBSAN bool memcmp(const void* blk1_, const void* blk2_, size_t count)
{
	const char *blk1 = (const char*)blk1_;
	const char *blk2 = (const char*)blk2_;
	for (size_t i = 0; i < count; i++)
		if (blk1[i] != blk2[i])
			return false;
	return true;
}
OBOS_NO_KASAN OBOS_NO_UBSAN bool memcmp_b(const void* blk1_, int against, size_t count)
{
	const uint8_t *blk1 = (const uint8_t*)blk1_;
	for (size_t i = 0; i < count; i++)
		if (blk1[i] != (uint8_t)against)
			return false;
	return true;
}
OBOS_NO_KASAN OBOS_NO_UBSAN bool strcmp(const char* str1, const char* str2)
{
	size_t sz1 = strlen(str1);
	size_t sz2 = strlen(str2);
	if (sz1 != sz2)
		return false;
	return memcmp(str1, str2, sz1);
}
OBOS_NO_KASAN OBOS_NO_UBSAN size_t strlen(const char* str)
{
	size_t i = 0;
	for (; str[i]; i++)
		;
	return i;
}
OBOS_NO_KASAN OBOS_NO_UBSAN size_t strchr(const char* str, char ch)
{
	size_t i = 0;
	for (; str[i] != ch && str[i]; i++)
		;
	return i + (str[i] == ch ? 1 : 0);
}