#pragma once
#define PacketProcessSignature(name) void Net_## name ## Process(vnode* nic, int depth, struct shared_ptr* buf, void* ptr, size_t size)
#define InvokePacketHandler(name, ptr, size) (Net_## name ## Process)(nic, depth + 1, OBOS_SharedPtrCopy(buf), ptr, size)