/*
 * oboskrnl/arch/x86_64/boot_info.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <UltraProtocol/ultra_protocol.h>

extern struct ultra_memory_map_attribute* Arch_MemoryMap;
extern struct ultra_platform_info_attribute* Arch_LdrPlatformInfo;
extern struct ultra_kernel_info_attribute* Arch_KernelInfo;
extern struct ultra_module_info_attribute* Arch_KernelBinary;
extern const char* OBOS_KernelCmdLine;