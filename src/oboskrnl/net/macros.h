#pragma once
#include <klog.h>
#include <cmdline.h>
#define PacketProcessSignature(name, data) OBOS_WEAK void Net_## name ## Process(vnode* nic, int depth, struct shared_ptr* buf, void* ptr, size_t size, data userdata)
#define PacketProcessSignature(name, data) OBOS_WEAK void Net_## name ## Process(vnode* nic, int depth, struct shared_ptr* buf, void* ptr, size_t size, data userdata)
#define InvokePacketHandler(name, ptr, size, data) (Net_## name ## Process)(nic, depth + 1, OBOS_SharedPtrCopy(buf), ptr, size, data)
#define ExitPacketHandler() do { OBOS_SharedPtrUnref(ptr); return; } while(0)
#define VerifyChecksum(data, size, chksum_func, chksum_member) \
({\
    typeof((data)->chksum_member) _remote_checksum = (data)->chksum_member;\
    (data)->chksum_member = 0;\
    typeof((data)->chksum_member) _our_checksum = chksum_func(data, size);\
    (data)->chksum_member = _remote_checksum;\
    (_remote_checksum == _our_checksum);\
})
#define NetError(...) \
if (!OBOS_GetOPTF("disable-network-error-logs"))\
    OBOS_Error(__VA_ARGS__);
#define NetUnimplemented(what) \
if (!OBOS_GetOPTF("disable-network-error-logs"))\
    OBOS_Warning("net: Unimplemented: " #what "\n");

#define DefineNetFreeSharedPtr \
static inline void NetFreeSharedPtr(shared_ptr *ptr) \
{\
    memset(ptr, 0xcc, sizeof(*ptr));\
    Free(OBOS_KernelAllocator, ptr, sizeof(*ptr));\
}
    
#if defined(__x86_64__)
#   define host_to_be16(val) __builtin_bswap16(val)
#   define host_to_be32(val) __builtin_bswap32(val)
#   define host_to_be64(val) __builtin_bswap64(val)
#   define be16_to_host(val) __builtin_bswap16(val)
#   define be32_to_host(val) __builtin_bswap32(val)
#   define be64_to_host(val) __builtin_bswap64(val)
#   define host_to_le16(val) (uint16_t)(val)
#   define host_to_le32(val) (uint32_t)(val)
#   define host_to_le64(val) (uint32_t)(val)
#   define le16_to_host(val) (uint16_t)(val)
#   define le32_to_host(val) (uint32_t)(val)
#   define le64_to_host(val) (uint64_t)(val)
#elif defined(__m68k__)
#   define host_to_be16(val) (uint16_t)(val)
#   define host_to_be32(val) (uint32_t)(val)
#   define host_to_be64(val) (uint32_t)(val)
#   define be16_to_host(val) (uint16_t)(val)
#   define be32_to_host(val) (uint32_t)(val)
#   define be64_to_host(val) (uint64_t)(val)
#   define host_to_le16(val) __builtin_bswap16(val)
#   define host_to_le32(val) __builtin_bswap32(val)
#   define host_to_le64(val) __builtin_bswap64(val)
#   define le16_to_host(val) __builtin_bswap16(val)
#   define le32_to_host(val) __builtin_bswap32(val)
#   define le64_to_host(val) __builtin_bswap64(val)
#else
#   error Define required macros.
#endif