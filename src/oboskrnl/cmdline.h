/*
 * oboskrnl/cmdline.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

extern OBOS_EXPORT const char* OBOS_KernelCmdLine;
extern OBOS_EXPORT const char* OBOS_InitrdBinary;
extern OBOS_EXPORT size_t OBOS_InitrdSize;
extern OBOS_EXPORT char** OBOS_argv;
extern OBOS_EXPORT size_t OBOS_argc;

// Parses the command line into OBOS_argv and OBOS_argc
void OBOS_ParseCMDLine();
// Gets the value of a string command line option.
OBOS_EXPORT char* OBOS_GetOPTS(const char* opt);
// Gets the value of an integer command line option.
OBOS_EXPORT uint64_t OBOS_GetOPTD(const char* opt);
// Gets the value of a flag command line option.
// true if the flag was found on the command line, otherwise false.
OBOS_EXPORT bool OBOS_GetOPTF(const char* opt);