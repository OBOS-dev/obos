/*
 * oboskrnl/vfs/socket.h
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * POSIX sockets
*/

#pragma once

#include <int.h>
#include <error.h>

#include <net/ip.h>

#include <vfs/fd.h>
#include <vfs/irp.h>

typedef struct sockaddr {
    unsigned short family;
    char data[14];
} sockaddr;
struct sockaddr_in {
	unsigned short family;
	uint16_t port;
	ip_addr addr;
	uint8_t sin_zero[8];
};
struct sockaddr_un {
	unsigned short sun_family;
	char sun_path[108];
};


#define IPPROTO_IP  0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

#define IP_TTL     2
#define IP_HDRINCL 3

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

#define AF_UNIX 1
#define AF_INET 2

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#define SOCK_CLOEXEC   02000000
#define SOCK_NONBLOCK  04000

// All flags are defined in linux headers for abi compatibility in mlibc
// If you need a flag, steal it from linux headers

obos_status Net_Socket(int domain, int type, int protocol, fd* out);
obos_status Net_Accept(fd* socket, sockaddr* addr, size_t* addr_len, int flags, fd* out);
obos_status Net_Bind(fd* socket, sockaddr* addr, size_t* addr_len);
obos_status Net_Connect(fd* socket, sockaddr* addr, size_t* addr_len);
obos_status Net_GetSockOpt(fd* socket, int level /* ignored */, int optname, void* optval, size_t *optlen);
obos_status Net_SetSockOpt(fd* socket, int level /* ignored */, int optname, const void* optval, size_t optlen);
obos_status Net_GetPeerName(fd* socket, sockaddr* addr, size_t* addr_len);
obos_status Net_GetSockName(fd* socket, sockaddr* addr, size_t* addr_len);
obos_status Net_Listen(fd* socket, int backlog);
obos_status Net_RecvFrom(fd* socket, void* buffer, size_t sz, int flags, size_t *nRead, sockaddr* addr, size_t* addr_len);
obos_status Net_SendTo(fd* socket, const void* buffer, size_t sz, int flags, size_t *nWritten, sockaddr* addr, size_t addr_len);
#define Net_Recv(socket,buffer,sz,flags,nRead) Net_RecvFrom(socket,buffer,sz,flags,nRead,nullptr,0)
#define Net_Send(socket,buffer,sz,flags,nWritten) Net_SendTo(socket,buffer,sz,flags,nWritten,nullptr,0)
obos_status Net_Shutdown(fd* desc, int how);
obos_status Net_SockAtMark(fd* desc);

#define MSG_OOB       0x0001
#define MSG_PEEK      0x0002
#define MSG_DONTROUTE 0x0004
#define MSG_CTRUNC    0x0008
#define MSG_PROXY     0x0010
#define MSG_TRUNC     0x0020
#define MSG_DONTWAIT  0x0040
#define MSG_EOR       0x0080
#define MSG_WAITALL   0x0100
#define MSG_FIN       0x0200
#define MSG_SYN       0x0400
#define MSG_CONFIRM   0x0800
#define MSG_RST       0x1000
#define MSG_ERRQUEUE  0x2000
#define MSG_NOSIGNAL  0x4000
#define MSG_MORE      0x8000
#define MSG_WAITFORONE 0x10000
#define MSG_BATCH     0x40000
#define MSG_ZEROCOPY  0x4000000
#define MSG_FASTOPEN  0x20000000
#define MSG_CMSG_CLOEXEC 0x40000000

typedef struct socket_desc {
    vnode* nic;
    vnode* vn;
    int protocol;
    void* protocol_data;
    void* local_ent;
	struct socket_ops* ops;
    size_t refs;
	struct {
		uint8_t ttl;
		bool hdrincl; 
	} opts;
} socket_desc;

typedef struct socket_ops {
	int domain;
	union {
		int protocol;
		int type;
	} proto_type;
	socket_desc*(*create)();
	void(*free)(socket_desc* socket);
	obos_status(*accept)(socket_desc* socket, struct sockaddr* addr, size_t* addrlen, int flags, bool nonblocking, socket_desc** out);
	obos_status(*bind)(socket_desc* socket, struct sockaddr* addr, size_t addrlen);
	obos_status(*connect)(socket_desc* socket, struct sockaddr* addr, size_t addrlen);
	obos_status(*getpeername)(socket_desc* socket, struct sockaddr* addr, size_t *addrlen);
	obos_status(*getsockname)(socket_desc* socket, struct sockaddr* addr, size_t *addrlen);
	obos_status(*listen)(socket_desc* socket, int backlog);
	obos_status(*submit_irp)(irp* req);
	obos_status(*finalize_irp)(irp* req);
	obos_status(*shutdown)(socket_desc* desc, int how);
	// OBOS_STATUS_SUCCESS if at OOB data mark, otherwise OBOS_STATUS_RETRY
	obos_status(*sockatmark)(socket_desc* desc);
} socket_ops;

obos_status NetH_AddSocketBackend(socket_ops* ops);

void VfsH_InitializeSocketInterface();