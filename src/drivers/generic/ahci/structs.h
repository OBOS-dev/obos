/*
	drivers/x86_64/sata/structs.h

	Copyright (c) 2023-2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <locks/semaphore.h>

#include <irq/irq.h>

#define	SATA_SIG_ATA	0x00000101	// SATA drive
#define	SATA_SIG_ATAPI	0xEB140101	// SATAPI drive
#define	SATA_SIG_SEMB	0xC33C0101	// Enclosure management bridge
#define	SATA_SIG_PM	0x96690101	// Port multiplier

#define HBA_PORT_IPM_ACTIVE 1
#define HBA_PORT_DET_PRESENT 3

typedef volatile struct HBA_PORT
{ 
	volatile uint32_t clb;		// 0x00, command list base address, 1K-byte aligned
	volatile uint32_t clbu;		// 0x04, command list base address upper 32 bits
	volatile uint32_t fb;		// 0x08, FIS base address, 256-byte aligned
	volatile uint32_t fbu;		// 0x0C, FIS base address upper 32 bits
	volatile uint32_t is;		// 0x10, interrupt status
	volatile uint32_t ie;		// 0x14, interrupt enable
	volatile uint32_t cmd;		// 0x18, command and status
	volatile uint32_t rsv0;		// 0x1C, Reserved
	volatile uint32_t tfd;		// 0x20, task file data
	volatile uint32_t sig;		// 0x24, signature
	volatile uint32_t ssts;		// 0x28, SATA status (SCR0:SStatus)
	volatile uint32_t sctl;		// 0x2C, SATA control (SCR2:SControl)
	volatile uint32_t serr;		// 0x30, SATA error (SCR1:SError)
	volatile uint32_t sact;		// 0x34, SATA active (SCR3:SActive)
	volatile uint32_t ci;		// 0x38, command issue
	volatile uint32_t sntf;		// 0x3C, SATA notification (SCR4:SNotification)
	volatile uint32_t fbs;		// 0x40, FIS-based switch control
	volatile uint32_t rsv1[11];	// 0x44 ~ 0x6F, Reserved
	volatile uint32_t vendor[4];	// 0x70 ~ 0x7F, vendor specific
} HBA_PORT;
typedef struct HBA_MEM
{
	// 0x00 - 0x2B, Generic Host Control
	volatile struct
	{
		const volatile uint8_t np : 5;
		const volatile bool sxs : 1;
		const volatile bool ems : 1;
		const volatile bool ccsc : 1;
		const volatile uint8_t nsc : 5;
		const volatile bool psc : 1;
		const volatile bool ssc : 1;
		const volatile bool pmd : 1;
		const volatile bool fbss : 1;
		const volatile bool spm : 1;
		const volatile bool sam : 1;
		const volatile bool resv1 : 1;
		const volatile uint8_t iss : 4;
		const volatile bool sclo : 1;
		const volatile bool sal : 1;
		const volatile bool salp : 1;
		const volatile bool sss : 1;
		const volatile bool smps : 1;
		const volatile bool ssntf : 1;
		const volatile bool sncq : 1;
		const volatile bool s64a : 1;
	} cap;		// 0x00, Host capability
	volatile struct 
	{
		// HBA Reset.
		volatile bool hr : 1;
		// Interrupt Enable.
		volatile bool ie : 1;
		// MSI Revert to Single Message
		volatile bool mrsm : 1;
		// Reserved.
		volatile uint32_t revs : 28;
		// AHCI Enable.
		volatile bool ae : 1;
	} __attribute__((packed)) ghc;		// 0x04, Global host control
	volatile uint32_t is;		// 0x08, Interrupt status
	volatile uint32_t pi;		// 0x0C, Port implemented
	volatile uint32_t vs;		// 0x10, Version
	volatile uint32_t ccc_ctl;	// 0x14, Command completion coalescing control
	volatile uint32_t ccc_pts;	// 0x18, Command completion coalescing ports
	volatile uint32_t em_loc;		// 0x1C, Enclosure management location
	volatile uint32_t em_ctl;		// 0x20, Enclosure management control
	volatile uint32_t cap2;		// 0x24, Host capabilities extended
	volatile uint32_t bohc;		// 0x28, BIOS/OS handoff control and status

	// 0x2C - 0x9F, Reserved
	volatile uint8_t  rsv[0xA0 - 0x2C];

	// 0xA0 - 0xFF, Vendor specific registers
	volatile uint8_t  vendor[0x100 - 0xA0];

	// 0x100 - 0x10FF, Port control registers
	volatile HBA_PORT	ports[32];	// 1 ~ 32
} HBA_MEM;

typedef enum FIS_TYPE
{
	FIS_TYPE_REG_H2D = 0x27,	// Register FIS - host to device
	FIS_TYPE_REG_D2H = 0x34,	// Register FIS - device to host
	FIS_TYPE_DMA_ACT = 0x39,	// DMA activate FIS - device to host
	FIS_TYPE_DMA_SETUP = 0x41,	// DMA setup FIS - bidirectional
	FIS_TYPE_DATA = 0x46,	// Data FIS - bidirectional
	FIS_TYPE_BIST = 0x58,	// BIST activate FIS - bidirectional
	FIS_TYPE_PIO_SETUP = 0x5F,	// PIO setup FIS - device to host
	FIS_TYPE_DEV_BITS = 0xA1,	// Set device bits FIS - device to host
} FIS_TYPE;

typedef struct FIS_REG_H2D
{
	volatile uint8_t fis_type; // FIS_TYPE_REG_H2D

	volatile uint8_t pmport : 4;
	volatile uint8_t rsv0 : 3;
	volatile uint8_t c : 1;

	volatile uint8_t command;
	volatile uint8_t featurel;

	volatile uint8_t lba0;
	volatile uint8_t lba1;
	volatile uint8_t lba2;
	volatile uint8_t device;

	volatile uint8_t lba3;
	volatile uint8_t lba4;
	volatile uint8_t lba5;
	volatile uint8_t featureh;

	volatile uint8_t countl;
	volatile uint8_t counth;
	volatile uint8_t icc;
	volatile uint8_t control;

	volatile uint8_t rsv1[4];
} FIS_REG_H2D;

typedef struct FIS_REG_D2H
{
	volatile uint8_t fis_type; // FIS_TYPE_REG_D2H

	volatile uint8_t pmport : 4;
	volatile uint8_t rsv0 : 2;
	volatile uint8_t i : 1;
	volatile uint8_t resv1 : 1;

	volatile uint8_t status;
	volatile uint8_t error;

	volatile uint8_t lba0;
	volatile uint8_t lba1;
	volatile uint8_t lba2;
	volatile uint8_t device;

	volatile uint8_t lba3;
	volatile uint8_t lba4;
	volatile uint8_t lba5;
	volatile uint8_t resv2;

	volatile uint8_t countl;
	volatile uint8_t counth;
	volatile uint8_t resv3[2];

	volatile uint8_t resv4[4];
} FIS_REG_D2H;

typedef struct FIS_DATA
{
	volatile uint8_t fis_type; // FIS_TYPE_DATA

	volatile uint8_t pmport : 4;
	volatile uint8_t rsv0 : 4;

	volatile uint8_t  rsv1[2];

	// DWORD 1 ~ N
	volatile uint8_t data[1];
} FIS_DATA;

typedef struct FIS_PIO_SETUP
{
	volatile uint8_t  fis_type; // FIS_TYPE_PIO_SETUP

	volatile uint8_t pmport : 4;
	volatile uint8_t rsv0 : 1;
	volatile uint8_t d : 1;
	volatile uint8_t i : 1;
	volatile uint8_t resv1 : 1;

	volatile uint8_t status;
	volatile uint8_t error;

	volatile uint8_t lba0;
	volatile uint8_t lba1;
	volatile uint8_t lba2;
	volatile uint8_t device;

	volatile uint8_t lba3;
	volatile uint8_t lba4;
	volatile uint8_t lba5;
	volatile uint8_t resv2;

	volatile uint8_t countl;
	volatile uint8_t counth;
	volatile uint8_t resv3;
	volatile uint8_t e_status;

	volatile uint16_t tc;
	volatile uint8_t  resv4[2];
} FIS_PIO_SETUP;

typedef struct FIS_DMA_SETUP
{
	uint8_t fis_type; // FIS_TYPE_DMA_SETUP

	volatile uint8_t pmport : 4;
	volatile uint8_t resv0 : 1;
	volatile uint8_t d : 1;
	volatile uint8_t i : 1;
	volatile uint8_t a : 1;

	volatile uint8_t resv1[2];

	volatile uint64_t DMAbufferID;

	volatile uint32_t resv2;

	volatile uint32_t DMAbufOffset;

	volatile uint32_t TransferCount;

	volatile uint32_t resv3;
} FIS_DMA_SETUP;

typedef volatile struct HBA_CMD_HEADER
{
	volatile uint8_t cfl:5;
	volatile uint8_t a:1;
	volatile uint8_t w:1;
	volatile uint8_t p:1;

	volatile uint8_t r:1;
	volatile uint8_t b:1;
	volatile uint8_t c:1;
	volatile uint8_t rsv0:1;
	volatile uint8_t pmp:4;
 
	volatile uint16_t prdtl;
 
	volatile uint32_t prdbc;
 
	volatile uint32_t ctba;
	volatile uint32_t ctbau;
 
	volatile uint32_t rsv1[4];	// Reserved
} HBA_CMD_HEADER;

typedef struct HBA_PRDT_ENTRY
{
	volatile uint32_t dba;
	volatile uint32_t dbau;
	volatile uint32_t rsv0;
 
	volatile uint32_t dbc:22;
	volatile uint32_t rsv1:9;
	volatile uint32_t i:1;
} HBA_PRDT_ENTRY;

typedef struct HBA_CMD_TBL
{
	volatile uint8_t cfis[64];

	volatile uint8_t acmd[16];
 
	volatile uint8_t rsv[48];
 
	volatile HBA_PRDT_ENTRY prdt_entry[32];
} HBA_CMD_TBL;

typedef enum drive_type
{
	DRIVE_TYPE_INVALID = 0,
	DRIVE_TYPE_SATA,
	DRIVE_TYPE_SATAPI,
} drive_type;
typedef struct Port
{
	uint8_t hbaPortIndex;
	volatile void* clBase;
	volatile void* fisBase;
	uintptr_t clBasePhys, fisBasePhys;
	uint32_t sectorSize; // used in get_blk_size
	uint64_t nSectors; // used in get_max_blk_count
	semaphore lock; // can have a maximum of 32 slots
	struct vnode* vn;
	const char* dev_name;
	struct command_data* PendingCommands[32];
	drive_type type;
	bool works;
} Port;

enum
{
	ATA_READ_DMA_EXT    = 0x25,
	ATA_WRITE_DMA_EXT   = 0x35,
	ATA_IDENTIFY_DEVICE = 0xEC,
};

extern volatile HBA_MEM* HBA;
// Arch-specific.
extern uint32_t HbaIrqNumber;
extern Port Ports[32];
extern size_t PortCount;
extern irq HbaIrq;

#if OBOS_IRQL_COUNT == 16
#	define IRQL_AHCI (7)
#elif OBOS_IRQL_COUNT == 8
#	define IRQL_AHCI (3)
#elif OBOS_IRQL_COUNT == 4
#	define IRQL_AHCI (2)
#elif OBOS_IRQL_COUNT == 2
#	define IRQL_AHCI (0)
#else
#	error Funny buisness.
#endif

#if OBOS_ARCHITECTURE_BITS == 64
#define AHCISetAddress(phys, field) \
do {\
    field = (phys & 0xffffffff);\
    if (HBA->cap.s64a)\
        field##u = (phys >> 32);\
} while(0)
#else
#define AHCISetAddress(phys, field) \
do {\
    field = (phys & 0xffffffff);\
} while(0)
#endif