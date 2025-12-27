/*
 * oboskrnl/vfs/socket.c
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * POSIX sockets
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <perm.h>

#include <net/ip.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <net/tables.h>
#include <net/macros.h>

#include <vfs/fd.h>
#include <vfs/socket.h>
#include <vfs/alloc.h>
#include <vfs/irp.h>
#include <vfs/local_socket.h>

#include <utils/shared_ptr.h>

#include <allocators/base.h>

struct {
    socket_ops** arr;
    int sz;
    bool type_is_idx : 1;
} Net_SocketBackendTable[AF_INET+1] = {
    [0]={},
    [AF_UNIX]={.type_is_idx=true},
    [AF_INET]={.type_is_idx=false},
};
#define BACKEND_TABLE_HAS_DOMAIN(domain) ((int)(domain) < (int)(sizeof(Net_SocketBackendTable)/sizeof((*Net_SocketBackendTable))))

static socket_ops* get_sock_ops(int domain, int type, int protocol)
{
    if (!BACKEND_TABLE_HAS_DOMAIN(domain))
        return nullptr;
    int idx = Net_SocketBackendTable[domain].type_is_idx ? type : protocol;
    if (idx >= Net_SocketBackendTable[domain].sz)
        return nullptr;
    return Net_SocketBackendTable[domain].arr[idx];
}

static obos_status get_blk_size(dev_desc desc, size_t* blkSize)
{
    OBOS_UNUSED(desc);
    *blkSize = 1;
    return OBOS_STATUS_SUCCESS;
}
static obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    OBOS_UNUSED(desc && count);
    return OBOS_STATUS_INVALID_OPERATION;
}

static obos_status submit_irp(void* req_)
{
    irp* req = req_;
    if (!req) 
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    socket_desc* desc = (void*)req->desc;
    if (!desc) 
        return OBOS_STATUS_INVALID_ARGUMENT;

    return desc->ops->submit_irp(req);
}

static obos_status finalize_irp(void* req_)
{
    irp* req = req_;
    if (!req)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    socket_desc* desc = (void*)req->desc;
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    if (desc->ops->finalize_irp)
        return desc->ops->finalize_irp(req);
    else
        return OBOS_STATUS_SUCCESS;
}

OBOS_PAGEABLE_FUNCTION static obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    OBOS_UNUSED(what);
    OBOS_UNUSED(request);
    OBOS_UNUSED(argp);
    return OBOS_STATUS_INVALID_IOCTL;
}
OBOS_PAGEABLE_FUNCTION static obos_status ioctl_argp_size(uint32_t request, size_t *sz)
{
    OBOS_UNUSED(request);
    OBOS_UNUSED(sz);
    return OBOS_STATUS_INVALID_IOCTL;
}

static obos_status reference_device(dev_desc* pdesc)
{
    if (!pdesc || !(*pdesc))
        return OBOS_STATUS_INVALID_ARGUMENT;
    socket_desc* desc = (void*)*pdesc;
    desc->refs++;
    return OBOS_STATUS_SUCCESS;
}
static obos_status unreference_device(dev_desc desc) 
{
    socket_desc* socket = (void*)desc;
    if (!socket)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(--socket->refs))
        socket->ops->free(socket);
    return OBOS_STATUS_SUCCESS;
}

static obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    socket_desc* socket = (void*)desc;
    irp* req = VfsH_IRPAllocate();
    req->desc = desc;
    req->vn = nullptr;
    req->blkOffset = blkOffset;
    req->blkCount = blkCount;
    req->buff = buf;
    req->op = IRP_READ;
    req->dryOp = false;
    req->socket_flags = 0;
    req->socket_data = NULL;
    req->sz_socket_data = 0;
    obos_status status = socket->ops->submit_irp(req);
    if (obos_is_error(status))
    {
        VfsH_IRPUnref(req);
        return status;
    }

    while (req->evnt)
    {
        obos_status status = Core_WaitOnObject(WAITABLE_OBJECT(*req->evnt));
        if (obos_is_error(status))
        {
            VfsH_IRPUnref(req);
            return status;
        }
        if (req->on_event_set)
            req->on_event_set(req);
        if (req->status != OBOS_STATUS_IRP_RETRY)
            break;
    }

    if (socket->ops->finalize_irp)
        socket->ops->finalize_irp(req);

    status = req->status;
    if (nBlkRead)
        *nBlkRead = req->nBlkRead;
    VfsH_IRPUnref(req);
    return status;
}
static obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    socket_desc* socket = (void*)desc;
    irp* req = VfsH_IRPAllocate();
    req->desc = desc;
    req->vn = nullptr;
    req->blkOffset = blkOffset;
    req->blkCount = blkCount;
    req->cbuff = buf;
    req->op = IRP_WRITE;
    req->dryOp = false;
    req->socket_flags = 0;
    req->socket_data = NULL;
    req->sz_socket_data = 0;
    obos_status status = socket->ops->submit_irp(req);
    if (obos_is_error(status))
    {
        VfsH_IRPUnref(req);
        return status;
    }

    while (req->evnt)
    {
        obos_status status = Core_WaitOnObject(WAITABLE_OBJECT(*req->evnt));
        if (obos_is_error(status))
        {
            VfsH_IRPUnref(req);
            return status;
        }
        if (req->on_event_set)
            req->on_event_set(req);
        if (req->status != OBOS_STATUS_IRP_RETRY)
            break;
    }

    if (socket->ops->finalize_irp)
        socket->ops->finalize_irp(req);

    status = req->status;
    if (nBlkWritten)
        *nBlkWritten = req->nBlkWritten;
    VfsH_IRPUnref(req);
    return status;
}

driver_id OBOS_SocketDriver = {
    .id=0,
    .header = {
        .magic=OBOS_DRIVER_MAGIC,
        .driverName="Socket Driver",
        .ftable = {
            .ioctl = ioctl,
            .ioctl_argp_size = ioctl_argp_size,
            .get_blk_size=get_blk_size,
            .read_sync=read_sync,
            .write_sync=write_sync,
            .get_max_blk_count=get_max_blk_count,
            .reference_device = reference_device,
            .unreference_device = unreference_device,
            .submit_irp = submit_irp,
            .finalize_irp = finalize_irp,
        },
    }
};
vdev OBOS_SocketDriverVdev = {
    .driver = &OBOS_SocketDriver,
};

static vnode* socket_make_vnode(int domain, int type, int protocol, socket_desc* idesc)
{
    vnode* vn = Vfs_Calloc(1, sizeof(vnode));
    vn->blkSize = 1;
    vn->filesize = 0;
    vn->vtype = VNODE_TYPE_SOCK;
    vn->desc = (dev_desc)(idesc ? idesc : get_sock_ops(domain, type, protocol)->create());
    vn->un.device = &OBOS_SocketDriverVdev;
    socket_desc *desc = (void*)vn->desc;
    desc->vn = vn;
    desc->opts.ttl = 64;
    desc->opts.hdrincl = false;
    return vn;
}

static void make_fd(fd* out, int domain, int type, int protocol, socket_desc* idesc)
{
    vnode* vn = socket_make_vnode(domain, type, protocol, idesc);
    socket_desc *desc = (void*)vn->desc;
    desc->protocol = protocol;
    desc->refs++;

    out->vn = vn;
    out->desc = out->vn->desc;
    out->flags = FD_FLAGS_OPEN|FD_FLAGS_READ|FD_FLAGS_WRITE|FD_FLAGS_UNCACHED;
    out->offset = 0;
    LIST_APPEND(fd_list, &out->vn->opened, out);
}

static obos_status inet_socket(int type, int protocol, fd* out)
{
    if (protocol == IPPROTO_IP && type == SOCK_DGRAM) protocol = IPPROTO_UDP;
    if (protocol == IPPROTO_IP && type == SOCK_STREAM) protocol = IPPROTO_TCP;

    if (protocol == IPPROTO_TCP && type != SOCK_STREAM)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (protocol == IPPROTO_UDP && type != SOCK_DGRAM)
        return OBOS_STATUS_INVALID_ARGUMENT;

    if (protocol == IPPROTO_TCP)
    {
        obos_status status = OBOS_CapabilityCheck("net/tcp", true);
        if (obos_is_error(status))
            return status;
    }
    if (protocol == IPPROTO_UDP)
    {
        obos_status status = OBOS_CapabilityCheck("net/udp", true);
        if (obos_is_error(status))
            return status;
    }

    if (!get_sock_ops(AF_INET, type, protocol))
        return OBOS_STATUS_INVALID_ARGUMENT;

    make_fd(out, AF_INET, type, protocol, nullptr);
    return OBOS_STATUS_SUCCESS;
}
static obos_status local_socket(int type, int protocol, fd* out)
{
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    if (!get_sock_ops(AF_UNIX, type, protocol))
        return OBOS_STATUS_INVALID_ARGUMENT;

    make_fd(out, AF_UNIX, type, protocol, nullptr);
    return OBOS_STATUS_SUCCESS;
}

#define validate_fd_status(fd)\
do {\
    if (~fd->flags & FD_FLAGS_OPEN)\
        return OBOS_STATUS_INVALID_ARGUMENT;\
    if (fd->vn->vtype != VNODE_TYPE_SOCK)\
        return OBOS_STATUS_INVALID_ARGUMENT;\
} while(0)

obos_status Net_Socket(int domain, int type, int protocol, fd* out)
{
    if (!out)
        return OBOS_STATUS_INVALID_ARGUMENT;
    int flags = type & (SOCK_CLOEXEC|SOCK_NONBLOCK);
    type &= ~(SOCK_CLOEXEC|SOCK_NONBLOCK);
    obos_status status = OBOS_STATUS_UNIMPLEMENTED;
    switch (domain) {
        case AF_INET:
            status = OBOS_CapabilityCheck("net/ipv4", true);
            if (obos_is_error(status))
                return status;
            status = inet_socket(type, protocol, out);
            break;
        case AF_UNIX:
            status = OBOS_CapabilityCheck("unix-socket", true);
            if (obos_is_error(status))
                return status;
            status = local_socket(type, protocol, out);
            break;
        default: break;
    }
    if (obos_is_success(status))
    {
        if (flags & SOCK_CLOEXEC)
            out->flags |= FD_FLAGS_NOEXEC;
        if (flags & SOCK_NONBLOCK)
            out->flags |= FD_FLAGS_NOBLOCK;
    }
    return status;
}

obos_status Net_Accept(fd* socket, sockaddr* oaddr, size_t* addr_len, int flags, fd* out)
{
    validate_fd_status(socket);
    socket_desc* desc = (void*)socket->vn->desc;
    if (!desc->ops->accept)
        return OBOS_STATUS_INVALID_OPERATION;
    obos_status status = OBOS_STATUS_UNIMPLEMENTED;
    if (desc->ops->domain == AF_INET)
    {
        status = OBOS_STATUS_SUCCESS;
        socket_desc* new_desc = nullptr;
        status = desc->ops->accept(desc, oaddr, addr_len, flags, socket->flags & FD_FLAGS_NOBLOCK, &new_desc);
        if (obos_is_error(status))
            return status;
        int type = 0;
        switch (desc->ops->proto_type.protocol) {
            case IPPROTO_TCP: type = SOCK_STREAM; break;
            case IPPROTO_UDP: type = SOCK_DGRAM; break;
            default: break;
        }
        make_fd(out, AF_INET, type, desc->ops->proto_type.protocol, new_desc);
    }
    else if (desc->ops->domain == AF_UNIX)
    {
        status = OBOS_STATUS_SUCCESS;
        socket_desc* new_desc = nullptr;
        status = desc->ops->accept(desc, oaddr, addr_len, flags, socket->flags & FD_FLAGS_NOBLOCK, &new_desc);
        if (obos_is_error(status))
            return status;
        int type = desc->ops->proto_type.type;
        make_fd(out, AF_UNIX, type, 0, new_desc);
    }
    if (flags & SOCK_NONBLOCK)
        out->flags |= FD_FLAGS_NOBLOCK;
    if (flags & SOCK_CLOEXEC)
        out->flags |= FD_FLAGS_NOEXEC;
    return status;
}

obos_status Net_Bind(fd* socket, sockaddr* addr, size_t* addr_len)
{
    validate_fd_status(socket);
    socket_desc* desc = (void*)socket->vn->desc;
    if (!desc->ops->bind)
        return OBOS_STATUS_INVALID_OPERATION;
    return desc->ops->bind(desc, addr, *addr_len);
}

obos_status Net_Connect(fd* socket, sockaddr* addr, size_t* addr_len)
{
    validate_fd_status(socket);
    socket_desc* desc = (void*)socket->vn->desc;
    if (!desc->ops->connect)
        return OBOS_STATUS_INVALID_OPERATION;
    return desc->ops->connect(desc, addr, *addr_len);
}

obos_status Net_SetSockOpt(fd* socket, int level /* ignored */, int optname, const void* optval, size_t optlen)
{
    validate_fd_status(socket);
    OBOS_UNUSED(level);
    socket_desc* desc = (void*)socket->vn->desc;
    if (desc->ops->domain != AF_INET)
        return OBOS_STATUS_INVALID_ARGUMENT;
    return OBOS_STATUS_INVALID_ARGUMENT;
    switch (optname) {
        case IP_TTL:
            if (optlen < sizeof(uint8_t))
                return OBOS_STATUS_INVALID_ARGUMENT;
            desc->opts.ttl = *(uint8_t*)optval;
            break;
        case IP_HDRINCL:
            if (optlen < sizeof(bool))
                return OBOS_STATUS_INVALID_ARGUMENT;
            desc->opts.hdrincl = *(bool*)optval;
            break;
        default: 
            OBOS_Warning("Unrecognized sockopt %d:%d\n", level, optname);
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status Net_GetSockOpt(fd* socket, int level /* ignored */, int optname, void* optval, size_t *optlen)
{
    validate_fd_status(socket);
    OBOS_UNUSED(level);
    socket_desc* desc = (void*)socket->vn->desc;
    if (desc->ops->domain != AF_INET)
        return OBOS_STATUS_INVALID_ARGUMENT;
    return OBOS_STATUS_INVALID_ARGUMENT;
    switch (optname) {
        case IP_TTL:
            if (*optlen < sizeof(uint8_t))
                return OBOS_STATUS_INVALID_ARGUMENT;
            *(uint8_t*)optval = desc->opts.ttl;
            *optlen = sizeof(uint8_t);
            break;
        case IP_HDRINCL:
            if (*optlen < sizeof(bool))
                return OBOS_STATUS_INVALID_ARGUMENT;
            *(bool*)optval = desc->opts.hdrincl;
            *optlen = sizeof(bool);
            break;
        default: 
            OBOS_Warning("%s: Unrecognized sockopt %d:%d.\n", __func__, level, optname);
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    return OBOS_STATUS_SUCCESS;
}

obos_status Net_GetPeerName(fd* socket, sockaddr* oaddr, size_t* addr_len)
{
    validate_fd_status(socket);
    socket_desc* desc = (void*)socket->vn->desc;
    if (!desc->ops->getpeername)
        return OBOS_STATUS_INVALID_OPERATION;
    return desc->ops->getpeername(desc, oaddr, addr_len);
}

obos_status Net_GetSockName(fd* socket, sockaddr* oaddr, size_t* addr_len)
{
    validate_fd_status(socket);
    socket_desc* desc = (void*)socket->vn->desc;
    if (!desc->ops->getsockname)
        return OBOS_STATUS_INVALID_OPERATION;
    return desc->ops->getsockname(desc, oaddr, addr_len);
}

obos_status Net_Listen(fd* socket, int backlog)
{
    validate_fd_status(socket);
    socket_desc* desc = (void*)socket->vn->desc;
    if (!desc->ops->listen)
        return OBOS_STATUS_INVALID_OPERATION;
    return desc->ops->listen(desc, backlog);
}

obos_status Net_RecvFrom(fd* socket, void* buffer, size_t sz, int flags, size_t *nRead, sockaddr* addr, size_t* len_addr)
{
    irp* req = VfsH_IRPAllocate();
    req->blkCount = sz;
    req->buff = buffer;
    req->socket_flags = flags;
    req->op = IRP_READ;
    req->dryOp = false;
    req->sz_socket_data = len_addr ? *len_addr : 0;
    req->socket_data = addr;
    req->vn = socket->vn;
    obos_status status = OBOS_STATUS_SUCCESS;
    if (obos_is_error(status = VfsH_IRPSubmit(req, &socket->desc)))
    {
        VfsH_IRPUnref(req);
        return status;
    }
    // if (((flags & MSG_DONTWAIT) || (socket->flags & FD_FLAGS_NOBLOCK)))
    //     printf("ohwowanonblockingsockethownice\n");
    if (socket->flags & FD_FLAGS_NOBLOCK)
    {
        if ((req->evnt && req->evnt->hdr.signaled) || !req->evnt)
            status = VfsH_IRPWait(req);
        else
            status = OBOS_STATUS_TIMED_OUT;
    }
    else
        status = VfsH_IRPWait(req);
    if (len_addr)
        *len_addr = sizeof(struct sockaddr_in);
    if (nRead)
        *nRead = req->nBlkRead;
    VfsH_IRPUnref(req);
    return status;
}

obos_status Net_SendTo(fd* socket, const void* buffer, size_t sz, int flags, size_t *nWritten, sockaddr* addr, size_t len_addr)
{
    irp* req = VfsH_IRPAllocate();
    req->blkCount = sz;
    req->cbuff = buffer;
    req->socket_flags = flags;
    req->op = IRP_WRITE;
    req->dryOp = false;
    req->sz_socket_data = len_addr;
    req->socket_data = addr;
    req->vn = socket->vn;
    obos_status status = OBOS_STATUS_SUCCESS;
    if (obos_is_error(status = VfsH_IRPSubmit(req, &socket->desc)))
    {
        VfsH_IRPUnref(req);
        return status;
    }
    status = VfsH_IRPWait(req);
    if (nWritten)
        *nWritten = req->nBlkWritten;
    VfsH_IRPUnref(req);
    return status;
}

obos_status Net_Shutdown(fd* socket, int how)
{
    validate_fd_status(socket);
    socket_desc* desc = (void*)socket->vn->desc;
    if (!desc->ops->shutdown)
        return OBOS_STATUS_INVALID_OPERATION;
    return desc->ops->shutdown(desc, how);
}

obos_status Net_SockAtMark(fd* socket)
{
    validate_fd_status(socket);
    socket_desc* desc = (void*)socket->vn->desc;
    if (!desc->ops->sockatmark)
        return OBOS_STATUS_INVALID_OPERATION;
    return desc->ops->sockatmark(desc);
}


obos_status NetH_AddSocketBackend(socket_ops* ops)
{
    if (!BACKEND_TABLE_HAS_DOMAIN(ops->domain))
    {
        OBOS_Warning("Attempted to add socket OPs for domain %d, while we do not support such a thing!\n", ops->domain);
        return OBOS_STATUS_INVALID_ARGUMENT;
    }
    if (Net_SocketBackendTable[ops->domain].sz < ops->proto_type.protocol)
    {
        Net_SocketBackendTable[ops->domain].sz = (ops->proto_type.protocol+3) & ~3;
        Net_SocketBackendTable[ops->domain].arr = Vfs_Realloc(Net_SocketBackendTable[ops->domain].arr, 
                                                               Net_SocketBackendTable[ops->domain].sz*sizeof(socket_ops));
    }
    Net_SocketBackendTable[ops->domain].arr[ops->proto_type.protocol] = ops;
    return OBOS_STATUS_SUCCESS;
}

void VfsH_InitializeSocketInterface()
{
    NetH_AddSocketBackend(&Net_UDPSocketBackend);
    NetH_AddSocketBackend(&Net_TCPSocketBackend);
    NetH_AddSocketBackend(&Vfs_LocalDgramSocketBackend);
    NetH_AddSocketBackend(&Vfs_LocalStreamSocketBackend);
}
