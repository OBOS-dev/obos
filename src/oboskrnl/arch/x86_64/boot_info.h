/*
 * oboskrnl/arch/x86_64/boot_info.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#if !OBOS_USE_LIMINE

#include <UltraProtocol/ultra_protocol.h>

extern volatile struct ultra_memory_map_attribute* Arch_MemoryMap;
extern volatile struct ultra_platform_info_attribute* Arch_LdrPlatformInfo;
extern volatile struct ultra_kernel_info_attribute* Arch_KernelInfo;
extern volatile struct ultra_module_info_attribute* Arch_KernelBinary;
extern volatile struct ultra_module_info_attribute* Arch_InitialSwapBuffer;
extern volatile struct ultra_framebuffer* Arch_Framebuffer;
extern volatile struct ultra_boot_context* Arch_BootContext;
#else

#include <limine.h>
#include <UltraProtocol/ultra_protocol.h>

extern volatile struct limine_framebuffer_request Arch_LimineFBRequest;
extern volatile struct limine_memmap_request Arch_LimineMemmapRequest;
extern volatile struct limine_module_request Arch_LimineModuleRequest;
extern volatile struct limine_hhdm_request Arch_LimineHHDMRequest;
extern volatile struct limine_executable_file_request Arch_LimineKernelInfoRequest;
extern volatile struct limine_executable_address_request Arch_LimineKernelAddressRequest;
extern volatile struct limine_executable_cmdline_request Arch_LimineKernelCmdlineRequest;
extern volatile struct limine_bootloader_info_request Arch_LimineBtldrInfoRequest;
extern volatile struct ultra_framebuffer* Arch_Framebuffer; // Artificially created
extern struct limine_file* Arch_KernelBinary;
#endif
extern uintptr_t Arch_RSDPBase;