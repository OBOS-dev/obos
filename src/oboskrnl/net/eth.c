/*
 * oboskrnl/net/eth.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <net/macros.h>
#include <net/eth.h>
#include <net/ip.h>
#include <net/arp.h>
#include <net/tables.h>

#include <allocators/base.h>

#include <utils/shared_ptr.h>

static bool initialized_crc32 = false;
static uint32_t crctab[256];

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
static OBOS_NO_UBSAN uint32_t crc32_bytes(const void *data, size_t sz)
{
    if (!initialized_crc32)
    {
        crcInit();
        initialized_crc32 = true;
    }
    return crc((char *)data, sz, ~0U);
}
__attribute__((alias("crc32_bytes"))) uint32_t NetH_CRC32Bytes(const void* data, size_t sz);

OBOS_NO_UBSAN 
PacketProcessSignature(Ethernet, void*)
{
    OBOS_UNUSED(userdata);
    OBOS_UNUSED(depth);
    ethernet2_header* hdr = ptr;
    if (~nic->flags & VFLAGS_NIC_NO_FCS)
    {
        uint32_t remote_checksum = le32_to_host(*(uint32_t*)((uintptr_t)ptr + size - 4));
        uint32_t our_checksum = crc32_bytes(ptr, size-4);
        if (remote_checksum != our_checksum)
        {
            NetError("%s: Wrong checksum in packet from " MAC_ADDRESS_FORMAT ". Expected checksum is 0x%08x, remote checksum is 0x%08x\n",
                __func__,
                MAC_ADDRESS_ARGS(hdr->src),
                our_checksum,
                remote_checksum
            );
            ExitPacketHandler();
        }
    }
    
    switch (be16_to_host(hdr->type)) {
        case ETHERNET2_TYPE_IPv4:
            InvokePacketHandler(IPv4, hdr+1, size-sizeof(ethernet2_header)-4/*CRC32*/, hdr);
            break;
        case ETHERNET2_TYPE_ARP:
            InvokePacketHandler(ARP, hdr+1, size-sizeof(ethernet2_header)-4/*CRC32*/, hdr);
            break;
        case ETHERNET2_TYPE_IPv6:
            NetUnimplemented(ETHERNET2_TYPE_IPv6);
            break;
        default:
            NetError("%s: Unrecognized ethernet header type 0x%04x from " MAC_ADDRESS_FORMAT "\n",
                __func__,
                be16_to_host(hdr->type),
                MAC_ADDRESS_ARGS(hdr->src));
            break;
    }

    ExitPacketHandler();
}

DefineNetFreeSharedPtr

OBOS_NO_UBSAN shared_ptr* NetH_FormatEthernetPacket(vnode* nic, const mac_address dest, const void* data, size_t size, uint16_t type)
{
    if (!nic->net_tables)
        return nullptr;
    if (nic->net_tables->magic != IP_TABLES_MAGIC)
        return nullptr;

    shared_ptr* buf = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(shared_ptr), nullptr);
    size_t real_size = size+4+sizeof(ethernet2_header);
    struct ethernet2_header* hdr = Allocate(OBOS_KernelAllocator, real_size, nullptr);
    OBOS_SharedPtrConstructSz(buf, hdr, real_size);
    buf->onDeref = NetFreeSharedPtr;
    buf->free = OBOS_SharedPtrDefaultFree;
    buf->freeUdata = OBOS_KernelAllocator;
    memcpy(hdr->dest, dest, sizeof(mac_address));
    memcpy(hdr->src, nic->net_tables->mac, sizeof(mac_address));
    hdr->type = host_to_be16(type);
    memcpy(hdr+1, data, size);
    uint32_t* checksum = (uint32_t*)((uintptr_t)hdr + real_size - 4);
    // this seems counterintuitive, that's because it is
    // but it's the only thing that works
    *checksum = host_to_le32(crc32_bytes(hdr, real_size-4)); 
    return buf;
}