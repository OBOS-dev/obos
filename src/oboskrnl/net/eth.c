/*
 * oboskrnl/net/eth.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>

#include <net/macros.h>
#include <net/eth.h>
#include <net/ip.h>
#include <net/arp.h>
#include <net/tables.h>

static bool initialized_crc32 = false;
static uint32_t crctab[256];

// For future reference, we cannot hardware-accelerate the crc32 algorithm as
// x86-64's crc32 uses a different polynomial than that of GPT.

static OBOS_NO_UBSAN void crcInit()
{
    uint32_t crc = 0;
    for (uint16_t i = 0; i < 256; ++i)
    {
        crc = i;
        for (uint8_t j = 0; j < 8; ++j)
        {
            uint32_t mask = -(crc & 1);
            crc = (crc >> 1) ^ (0xEDB88320 & mask);
        }
        crctab[i] = crc;
    }
}
static OBOS_NO_UBSAN uint32_t crc(const char *data, size_t len, uint32_t result)
{
    for (size_t i = 0; i < len; ++i)
        result = (result >> 8) ^ crctab[(result ^ data[i]) & 0xFF];
    return ~result;
}
static OBOS_NO_UBSAN uint32_t crc32_bytes_from_previous(void *data, size_t sz,
                                   uint32_t previousChecksum)
{
    if (!initialized_crc32)
    {
        crcInit();
        initialized_crc32 = true;
    }
    return crc((char *)data, sz, ~previousChecksum);
}
static OBOS_NO_UBSAN uint32_t crc32_bytes(void *data, size_t sz)
{
    if (!initialized_crc32)
    {
        crcInit();
        initialized_crc32 = true;
    }
    return crc((char *)data, sz, ~0U);
}

PacketProcessSignature(Ethernet, void*)
{
    OBOS_UNUSED(userdata);
    ethernet2_header* hdr = ptr;
    // In case of wacky NICs, check if the MAC address destination is:
    // nic->mac
    // or MAC_BROADCAST
    uint32_t remote_checksum = *(uint32_t*)((uintptr_t)ptr + size - 4);
    uint32_t our_checksum = crc32_bytes(ptr, size-4);
    if (remote_checksum != our_checksum)
    {
        NetError("%s: Wrong checksum in packet from " MAC_ADDRESS_FORMAT "Expected checksum is 0x%08x, remote checksum is 0x%08x\n",
            __func__,
            MAC_ADDRESS_ARGS(hdr),
            our_checksum,
            remote_checksum
        );
        return;
    }
    
    // Verify CRC32
    switch (hdr->type) {
        case ETHERNET2_TYPE_IPv4:
            InvokePacketHandler(IPv4, hdr+1, size-sizeof(ethernet2_header)-4/*CRC32*/, hdr);
            break;
        case ETHERNET2_TYPE_ARP:
            InvokePacketHandler(ARP, hdr+1, size-sizeof(ethernet2_header)-4/*CRC32*/, hdr);
            break;
        case ETHERNET2_TYPE_IPv6:
            break;
        default:
            NetError("%s: Unrecognized ethernet header type 0x%04x from " MAC_ADDRESS_FORMAT "\n",
                __func__,
                hdr->type,
                MAC_ADDRESS_ARGS(hdr));
            break;
    }
}