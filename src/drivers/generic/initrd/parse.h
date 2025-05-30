/*
 * drivers/generic/initrd/parse.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include "ustar_hdr.h"

extern initrd_inode* InitrdRoot;
extern size_t CurrentInodeNumber;

const ustar_hdr* GetFile(const char* path, obos_status* status);
initrd_inode* DirentLookupFrom(const char* path, initrd_inode* root);
inline static uint64_t oct2bin(const char* str, size_t size)
{
	uint64_t n = 0;
	const char* c = str;
	while (size-- > 0) 
		n = n * 8 + (uint64_t)(*c++ - '0');
	return n;
}
void* malloc(size_t sz);
void* realloc(void* buf, size_t sz);
void free(void* buf);
