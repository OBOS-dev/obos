/*
 * oboskrnl/cmdline.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

extern const char* OBOS_KernelCmdLine;
extern char** OBOS_argv;
extern size_t OBOS_argc;

// Parses the command line into OBOS_argv and OBOS_argc
void OBOS_ParseCMDLine();
// Gets the value of a string command line option.
char* OBOS_GetOPTS(const char* opt);
// Gets the value of an integer command line option.
uint64_t OBOS_GetOPTD(const char* opt);
// Gets the value of a flag command line option.
// true if the flag was found on the command line, otherwise false.
bool OBOS_GetOPTF(const char* opt);