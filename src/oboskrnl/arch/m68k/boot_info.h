/*
 * oboskrnl/arch/m68k/boot_info.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <struct_packing.h>

typedef enum BootInfoType
{
    BootInfoType_Last = 0,
    BootInfoType_MachType = 1,
    BootInfoType_CpuType = 2,
    BootInfoType_FpuType = 3,
    BootInfoType_MmuType = 4,
    BootInfoType_MemChunk = 5,
    BootInfoType_InitRd = 6,
    BootInfoType_CommandLine = 7,
    BootInfoType_RngSeed = 8,

    BootInfoType_QemuVersion = 0x8000,
    BootInfoType_GoldfishPicBase = 0x8001,
    BootInfoType_GoldfishRtcBase = 0x8002,
    BootInfoType_GoldfishTtyBase = 0x8003,
    BootInfoType_VirtioBase = 0x8004,
    BootInfoType_ControlBase = 0x8005
} BootInfoType;

typedef struct BootDeviceBase
{
    uint32_t base;
    uint32_t irq;
} BootDeviceBase;
typedef struct BootInfoTag
{
    uint16_t type;
    uint16_t size;
} OBOS_PACK BootInfoTag;

OBOS_EXPORT BootInfoTag* Arch_GetBootInfo(BootInfoType type);
OBOS_EXPORT BootInfoTag* Arch_GetBootInfoFrom(BootInfoType type, BootInfoTag* tag);