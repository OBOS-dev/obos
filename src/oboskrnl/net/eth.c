/*
 * oboskrnl/net/eth.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <net/eth.h>

#include <allocators/base.h>

static bool initialized_crc32 = false;
static uint32_t crctab[256];

// For future reference, we cannot hardware-accelerate the crc32 algorithm as
// x86-64's crc32 uses a different polynomial than that of GPT.

static void crcInit() {
  uint32_t crc = 0;
  for (uint16_t i = 0; i < 256; ++i) {
    crc = i;
    for (uint8_t j = 0; j < 8; ++j) {
      uint32_t mask = -(crc & 1);
      crc = (crc >> 1) ^ (0xEDB88320 & mask);
    }
    crctab[i] = crc;
  }
}
static uint32_t crc(const char *data, size_t len, uint32_t result) {
  for (size_t i = 0; i < len; ++i)
    result = (result >> 8) ^ crctab[(result ^ data[i]) & 0xFF];
  return ~result;
}
static uint32_t crc32_bytes_from_previous(void *data, size_t sz,
                                   uint32_t previousChecksum) {
  if (!initialized_crc32) {
    crcInit();
    initialized_crc32 = true;
  }
  return crc((char *)data, sz, ~previousChecksum);
}
static uint32_t crc32_bytes(void *data, size_t sz)
{
  if (!initialized_crc32) {
    crcInit();
    initialized_crc32 = true;
  }
  return crc((char *)data, sz, ~0U);
}

obos_status Net_FormatEthernet2Packet(ethernet2_header** phdr, void* data, size_t sz, const mac_address* restrict dest, const mac_address* restrict src, uint16_t type, size_t *out_sz)
{
    if (!phdr || !data || !sz || !src || !dest || !out_sz)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ethernet2_header* hdr = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(ethernet2_header)+sz+4, nullptr);
    memcpy(hdr->dest, dest, sizeof(mac_address));
    memcpy(hdr->src, src, sizeof(mac_address));
    hdr->type = host_to_be16(type);
    memcpy(hdr+1, data, sz);
    *(uint32_t*)(&((uint8_t*)hdr)[sizeof(ethernet2_header) + sz]) = crc32_bytes(hdr, sizeof(ethernet2_header) + sz);
    *out_sz = sizeof(ethernet2_header)+sz+4;
    *phdr = hdr;
    return OBOS_STATUS_SUCCESS;
}
