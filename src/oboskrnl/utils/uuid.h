/*
 * oboskrnl/utils/uuid.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <utils/string.h>

typedef uint32_t uuid[4];
OBOS_STATIC_ASSERT(sizeof(uuid) == 16, "The size of a UUID is not 16 bytes (128-bits)!");

// str is a pointer to a newly zeroed string object
OBOS_EXPORT void OBOS_UUIDToString(const uuid* const uuid, string* str);
OBOS_EXPORT void OBOS_StringToUUID(const string* const str, uuid* uuid);
// TODO: Generate UUID?