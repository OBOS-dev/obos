#pragma once
#include <klog.h>
#include <cmdline.h>
#define PacketProcessSignature(name, data) OBOS_WEAK void Net_## name ## Process(vnode* nic, int depth, struct shared_ptr* buf, void* ptr, size_t size, data userdata)
#define InvokePacketHandler(name, ptr, size, data) (Net_## name ## Process)(nic, depth + 1, OBOS_SharedPtrCopy(buf), ptr, size, data)
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