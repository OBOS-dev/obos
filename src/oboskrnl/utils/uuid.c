/*
 * oboskrnl/utils/uuid.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>
#include <klog.h>

#include <utils/string.h>
#include <utils/uuid.h>

static uint64_t hex2bin(const char* str, size_t size);
static uint32_t uuid2host(uint32_t i)
{
	if (strcmp(OBOS_ARCHITECTURE_ENDIANNESS, "Big-Endian"))
		return __builtin_bswap32(i);
	return i;
}
static uint32_t host2uuid(uint32_t i)
{
	if (strcmp(OBOS_ARCHITECTURE_ENDIANNESS, "Big-Endian"))
		return __builtin_bswap32(i);
	return i;
}
void OBOS_UUIDToString(const uuid* const uuid, string* out)
{
	string str = {};
	// size_t str_len = snprintf(nullptr, 0, "%08x-%04x-%04x-%04x-%04x%08x", 
	//     (*uuid)[0], 
	//     (*uuid)[1] >> 16, (*uuid)[1] & 0xffff, 
	//     (*uuid)[2] >> 16, (*uuid)[2] & 0xffff, 
	//     (*uuid)[3]);
	size_t str_len = 36;
	OBOS_InitString(&str, "");
	OBOS_ResizeString(&str, str_len);
	if (strcmp(OBOS_ARCHITECTURE_ENDIANNESS, "Little-Endian"))
	{
		snprintf(OBOS_GetStringPtr(&str), str_len+1, "%08x-%04x-%04x-%04x-%04x%08x", 
			(*uuid)[0], 
			(*uuid)[1] & 0xffff, (*uuid)[1] >> 16, 
			__builtin_bswap16((*uuid)[2] & 0xffff), __builtin_bswap16((*uuid)[2] >> 16), 
			__builtin_bswap32((*uuid)[3]));
	}
	else
	{
		snprintf(OBOS_GetStringPtr(&str), str_len+1, "%08x-%04x-%04x-%04x-%04x%08x", 
			__builtin_bswap32((*uuid)[0]), 
			__builtin_bswap32((*uuid)[1]) & 0xffff, __builtin_bswap32((*uuid)[1]) >> 16, 
			__builtin_bswap32((*uuid)[2]) & 0xffff, __builtin_bswap32((*uuid)[2]) >> 16, 
			__builtin_bswap32((*uuid)[3]));
	}
	*out = str;
}
void OBOS_StringToUUID(const string* const str, uuid* out)
{
	size_t str_len = OBOS_GetStringSize(str);
	if (str_len < 36)
		return;
	const char* iter = OBOS_GetStringCPtr(str);
	(*out)[0] = hex2bin(iter, 8);
	iter += 8;
	iter += 1;
	(*out)[1] = hex2bin(iter, 4) << 16;
	iter += 4;
	iter += 1;
	(*out)[1] |= hex2bin(iter, 4);
	iter += 4;
	iter += 1;
	(*out)[2] = hex2bin(iter, 4) << 16;
	iter += 4;
	iter += 1;
	(*out)[2] |= hex2bin(iter, 4);
	iter += 4;
	(*out)[3] |= hex2bin(iter, 8);
	for (int i = 0; i < 4; i++)
		(*out)[i] = host2uuid((*out)[i]);
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