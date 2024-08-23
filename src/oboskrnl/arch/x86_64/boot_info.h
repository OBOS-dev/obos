/*
 * oboskrnl/arch/x86_64/boot_info.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <UltraProtocol/ultra_protocol.h>

extern volatile struct ultra_memory_map_attribute* Arch_MemoryMap;
extern volatile struct ultra_platform_info_attribute* Arch_LdrPlatformInfo;
extern volatile struct ultra_kernel_info_attribute* Arch_KernelInfo;
extern volatile struct ultra_module_info_attribute* Arch_KernelBinary;
extern volatile struct ultra_module_info_attribute* Arch_InitialSwapBuffer;
extern volatile struct ultra_framebuffer* Arch_Framebuffer;
extern volatile struct ultra_boot_context* Arch_BootContext;