/*
 * drivers/generic/nvme/structs.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

typedef volatile struct nvme_base_registers {
    uint64_t cap;
    uint32_t version;
    uint32_t intms;
    uint32_t intmc;
    uint32_t cc;
    uint32_t resv1;
    uint32_t csts;
    uint32_t nssr;
    uint32_t aqa;
    uint64_t asq; // admin submission queue
    uint64_t acq; // admin completion queue
    uint32_t cmbloc;
    uint32_t cmblz;
    uint32_t bpinfo;
    uint32_t bprsel;
    uint64_t bpmbl;
    uint64_t cmbmsc;
    uint32_t cmbsts;
    uint32_t cmbebs;
    uint32_t cmbswtp;
    uint32_t nssd;
    uint32_t crto;
    char resv2[0xD94];
    uint32_t pmrcap;
    uint32_t pmrctl;
    uint32_t pmrsts;
    uint32_t pmrebs;
    uint32_t pmrswtp;
    uint32_t pmrmscl;
    uint32_t pmrmscu;
} OBOS_PACK nvme_base_registers;

enum {
    NVME_PSDT_PRP = 0b00,
    NVME_PSDT_SGL = 0b01,
    NVME_PSDT_SGL_SEG = 0b10,
};

typedef struct nvme_submission_queue_entry {
    union {
        struct {
            uint8_t opcode;
            // Last 2 bits are used for PRP/SGL selection
            uint8_t unused1;
            uint16_t cid;
        };
        uint32_t dword0;
    };
    uint32_t nsid;
    uint32_t resv1[2];
    uint64_t metadata_ptr;
    uint64_t data_ptr[2];
    uint32_t cmd_specific[6];
} OBOS_PACK nvme_submission_queue_entry;

typedef struct nvme_completion_queue_entry {
    uint32_t cmd_specific;
    uint32_t resv;
    uint32_t submission_queue_head;
    uint32_t submission_queue_id;
    uint16_t cmd_id;
    // bit 0: Phase bit
    // bits 1-15: status field
    uint16_t status_phase; 
} OBOS_PACK nvme_completion_queue_entry;

// cring
typedef struct nvme_prp_entry {
    uint64_t address;
} OBOS_PACK nvme_prp_entry;

enum {
    NVME_SGL_DATA_BLOCK,
    NVME_SGL_BIT_BUCKET,
    NVME_SGL_SEGMENT,
    NVME_SGL_LAST_SEGMENT,
};

// Scatter-gather data block
typedef struct nvme_sgl_data_block {
    uint64_t address;
    uint32_t length;
    uint8_t resv[3];
    uint8_t sglid;
} OBOS_PACK nvme_sgl_data_block;