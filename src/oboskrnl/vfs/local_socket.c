/*
 * oboskrnl/vfs/local_socket.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>

#include <vfs/socket.h>
#include <vfs/local_socket.h>
#include <vfs/alloc.h>
#include <vfs/create.h>
#include <vfs/mount.h>

#include <scheduler/schedule.h>
#include <scheduler/process.h>

#include <mm/alloc.h>

#include <locks/mutex.h>
#include <locks/event.h>

#include <utils/list.h>

struct dgram_pckt {
    void* buff;
    size_t sz;
    LIST_NODE(dgram_pckt_list, struct dgram_pckt) node;
};
typedef LIST_HEAD(dgram_pckt_list, struct dgram_pckt) dgram_pckt_list;
LIST_GENERATE_STATIC(dgram_pckt_list, struct dgram_pckt, node);

struct ringbuffer {
    void* buffer;
    size_t size;
    size_t in_ptr;
    size_t ptr;
    event doorbell;
    event empty;
    mutex lock;
};
static obos_status ringbuffer_write(struct ringbuffer* buf, const void* buffer, size_t sz, size_t *bytes_written)
{
    if (!buf || !buffer)
        return OBOS_STATUS_INVALID_ARGUMENT;
    Core_MutexAcquire(&buf->lock);
    if ((sz+buf->ptr) >= buf->size)
        sz = buf->size - buf->ptr;
    void* out_ptr = (void*)((uintptr_t)buf->buffer + buf->ptr);    
    memcpy(out_ptr, buffer, sz);
    buf->ptr += sz;
    Core_EventSet(&buf->doorbell, false);
    Core_EventClear(&buf->empty);
    if (bytes_written)
        *bytes_written = sz;
    Core_MutexRelease(&buf->lock);
    return OBOS_STATUS_SUCCESS;
}
static obos_status ringbuffer_ready_count(struct ringbuffer* stream, size_t* bytes_ready)
{
    if (!bytes_ready || !stream)
        return OBOS_STATUS_INVALID_ARGUMENT;
    Core_MutexAcquire(&stream->lock);
    *bytes_ready = stream->ptr - stream->in_ptr;
    Core_MutexRelease(&stream->lock);
    return OBOS_STATUS_SUCCESS;
}
static obos_status ringbuffer_read(struct ringbuffer* stream, void* buffer, size_t sz, size_t* bytes_read, bool peek)
{
    if (!stream || !buffer)
        return OBOS_STATUS_INVALID_ARGUMENT;
    Core_MutexAcquire(&stream->lock);
    const void* in_ptr = (void*)((uintptr_t)stream->buffer + stream->in_ptr);    
    memcpy(buffer, in_ptr, sz);
    if (!peek)
    {
        stream->in_ptr += sz;
        stream->ptr -= sz;
        if (stream->ptr <= 0)
        {
            // Clear the data event, and set the pipe empty event.
            stream->in_ptr = 0;
            stream->ptr = 0;
            Core_EventSet(&stream->empty, false);
            Core_EventClear(&stream->doorbell);
        }
        Core_EventSet(&stream->doorbell, false);
    }
    if (bytes_read)
        *bytes_read = sz;
    Core_MutexRelease(&stream->lock);
    return OBOS_STATUS_SUCCESS;
}
static void ringbuffer_initialize(struct ringbuffer* stream)
{
    stream->lock = MUTEX_INITIALIZE();
    stream->doorbell = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    stream->empty = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    stream->size = OBOS_PAGE_SIZE*16;
    stream->buffer = Mm_QuickVMAllocate(stream->size, false);
    stream->ptr = stream->in_ptr = 0;

    Core_EventSet(&stream->empty, false);
}
static void ringbuffer_free(struct ringbuffer* stream)
{
    Mm_VirtualMemoryFree(&Mm_KernelContext, stream->buffer, stream->size);
    CoreH_AbortWaitingThreads(WAITABLE_OBJECT(stream->doorbell));
    CoreH_AbortWaitingThreads(WAITABLE_OBJECT(stream->empty));
}

struct open_local_socket {
    union {
        struct {
            dgram_pckt_list inbound;
            dgram_pckt_list outbound;
            mutex inbound_lock;
            mutex outbound_lock;
            event inbound_doorbell;
            event outbound_doorbell;
        } dgram;
        struct {
            struct ringbuffer server_bound;
            struct ringbuffer client_bound;
        } stream;
    };
    struct local_socket* server;
    dirent* bound_ent;
    struct local_socket* lsckt;
    LIST_NODE(open_local_socket_list, struct open_local_socket) node;
};
typedef LIST_HEAD(open_local_socket_list, struct open_local_socket) open_local_socket_list;
LIST_GENERATE_STATIC(open_local_socket_list, struct open_local_socket, node);
struct server_local_socket {
    open_local_socket_list waiting_clients;
    event doorbell;
    mutex lock;
    dirent* file;
};
struct local_socket {
    bool is_server : 1;
    // If this is false, then the 'serv' field is valid
    bool is_open : 1;
    union {
        struct server_local_socket serv;
        struct open_local_socket* open;
    };
    union {
        struct ringbuffer* incoming_stream;
        struct {
            dgram_pckt_list* pckts;
            mutex* lock;
            event* doorbell;
        } incoming_dgram;
    };
    union {
        struct ringbuffer* outgoing_stream;
        struct {
            dgram_pckt_list* pckts;
            mutex* lock;
            event* doorbell;
        } outgoing_dgram;
    };
    struct local_socket* peer;
};

/*****************************************/
/********* UNIX Datagram Sockets *********/
/*****************************************/

static socket_desc* dgram_create();
static void dgram_free(socket_desc* socket)
{
    Vfs_Free(socket->protocol_data);
    Vfs_Free(socket);
}

static obos_status dgram_bind(socket_desc* socket, struct sockaddr* addr, size_t addrlen);
static obos_status dgram_connect(socket_desc* socket, struct sockaddr* addr, size_t addrlen);
static obos_status dgram_getpeername(socket_desc* socket, struct sockaddr* addr, size_t *addrlen);
static obos_status dgram_getsockname(socket_desc* socket, struct sockaddr* addr, size_t *addrlen);
static obos_status dgram_submit_irp(irp* req);
static obos_status dgram_finalize_irp(irp* req);

socket_ops Vfs_LocalDgramSocketBackend = {
    .domain = AF_UNIX,
    .proto_type.type = SOCK_DGRAM,
    .create = dgram_create,
    .free = dgram_free,
    .accept = nullptr,
    .bind = dgram_bind,
    .connect = dgram_connect,
    .getpeername = dgram_getpeername,
    .getsockname = dgram_getsockname,
    .listen = nullptr,
    .submit_irp = dgram_submit_irp,
    .finalize_irp = dgram_finalize_irp,
    .shutdown = nullptr,
    .sockatmark = nullptr,
};

static socket_desc* dgram_create()
{
    socket_desc* desc = Vfs_Calloc(1, sizeof(socket_desc));
    desc->ops = &Vfs_LocalDgramSocketBackend;
    desc->protocol = SOCK_DGRAM;
    desc->protocol_data = nullptr;
    return desc;
}

static obos_status dgram_bind(socket_desc* socket, struct sockaddr* addr, size_t addrlen)
{
    OBOS_UNUSED(socket && addr && addrlen);
    return OBOS_STATUS_UNIMPLEMENTED;
}
static obos_status dgram_connect(socket_desc* socket, struct sockaddr* addr, size_t addrlen)
{
    OBOS_UNUSED(socket && addr && addrlen);
    return OBOS_STATUS_UNIMPLEMENTED;
}
static obos_status dgram_getpeername(socket_desc* socket, struct sockaddr* addr, size_t *addrlen)
{
    OBOS_UNUSED(socket && addr && addrlen);
    return OBOS_STATUS_UNIMPLEMENTED;
}
static obos_status dgram_getsockname(socket_desc* socket, struct sockaddr* addr, size_t *addrlen)
{
    OBOS_UNUSED(socket && addr && addrlen);
    return OBOS_STATUS_UNIMPLEMENTED;
}
static obos_status dgram_submit_irp(irp* req)
{
    OBOS_UNUSED(req);
    return OBOS_STATUS_UNIMPLEMENTED;
}
static __attribute__((alias("dgram_submit_irp"))) obos_status dgram_finalize_irp(irp* req);

/*****************************************/
/********** UNIX Stream Sockets **********/
/*****************************************/

static size_t strrfind(const char* str, char ch)
{
    int64_t i = strlen(str);
    for (; i >= 0; i--)
        if (str[i] == ch)
           return i;
    return SIZE_MAX;
}

static socket_desc* stream_create();
static void stream_free(socket_desc* socket);

static obos_status stream_bind(socket_desc* socket, struct sockaddr* addr, size_t addrlen);
static obos_status stream_accept(socket_desc* socket, struct sockaddr* addr, size_t* addrlen, int flags, bool nonblocking, socket_desc** out);
static obos_status stream_connect(socket_desc* socket, struct sockaddr* addr, size_t addrlen);
static obos_status stream_getpeername(socket_desc* socket, struct sockaddr* addr, size_t *addrlen);
static obos_status stream_getsockname(socket_desc* socket, struct sockaddr* addr, size_t *addrlen);
static obos_status stream_listen(socket_desc* socket, int backlog);
static obos_status stream_shutdown(socket_desc* desc, int how);
static obos_status stream_submit_irp(irp* req);

socket_ops Vfs_LocalStreamSocketBackend = {
    .domain = AF_UNIX,
    .proto_type.type = SOCK_STREAM,
    .create = stream_create,
    .free = stream_free,
    .accept = stream_accept,
    .bind = stream_bind,
    .connect = stream_connect,
    .getpeername = stream_getpeername,
    .getsockname = stream_getsockname,
    .listen = stream_listen,
    .submit_irp = stream_submit_irp,
    .finalize_irp = nullptr,
    .shutdown = stream_shutdown,
    .sockatmark = nullptr,
};

static socket_desc* stream_create()
{
    socket_desc* desc = Vfs_Calloc(1, sizeof(socket_desc));
    desc->ops = &Vfs_LocalStreamSocketBackend;
    desc->protocol = SOCK_STREAM;
    desc->protocol_data = nullptr;
    return desc;
}

static void stream_free(socket_desc* socket)
{
    if (!socket->protocol_data)
        return;
    struct local_socket* lsckt = socket->protocol_data;
    if (lsckt->peer)
    {
        // Close the peer's connection.
        lsckt->peer->peer = nullptr;
        lsckt->peer->incoming_stream = nullptr;
        lsckt->peer->outgoing_stream = nullptr;
        // Free the buffers
        ringbuffer_free(&lsckt->open->stream.client_bound);
        ringbuffer_free(&lsckt->open->stream.server_bound);
    }
    else
    {
        if (lsckt->is_open)
            Vfs_Free(lsckt->open);
        Vfs_Free(socket->protocol_data);
    }
    socket->protocol_data = nullptr;
    // Free 'socket' on deletion
}

static obos_status stream_bind(socket_desc* socket, struct sockaddr* addr, size_t addrlen)
{
    if (addrlen == sizeof(addr->family))
        return OBOS_STATUS_UNIMPLEMENTED; // unnamed local sockets are unimplemented.
    if (addrlen == (sizeof(addr->family)+1))
        return OBOS_STATUS_UNIMPLEMENTED; // abstract local sockets are unimplemented and a linux extension.
    if (addrlen > sizeof(struct sockaddr_un))
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (socket->protocol_data)
        return OBOS_STATUS_ALREADY_INITIALIZED;

    struct sockaddr_un cpy_addr = {};
    memcpy(&cpy_addr, addr, addrlen);
    char* name = cpy_addr.sun_path;

    dirent* parent = *name == '/' ? Vfs_Root : Core_GetCurrentThread()->proc->cwd;
    size_t index = strrfind(name, '/');
    char* dirname = name;
    if (index != SIZE_MAX)
    {
        dirname = name+index;
        *dirname = 0;
        dirname++;
        parent = VfsH_DirentLookupFrom(name, parent);
    }
    if (!parent)
        return OBOS_STATUS_NOT_FOUND;
    if (VfsH_DirentLookupFrom(dirname, parent))
        return OBOS_STATUS_ADDRESS_IN_USE;

    vnode* vn = socket->vn;
    dirent* ent = Vfs_Calloc(1, sizeof(dirent));
    OBOS_InitString(&ent->name, dirname);
    ent->vnode = vn;
    VfsH_DirentAppendChild(parent, ent);
    socket->local_ent = ent;

    return OBOS_STATUS_SUCCESS;
}

static obos_status stream_accept(socket_desc* socket, struct sockaddr* addr, size_t* addrlen, int flags, bool nonblocking, socket_desc** out)
{
    OBOS_UNUSED(flags);
    if (!socket)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!socket->protocol_data || !socket->local_ent)
        return OBOS_STATUS_UNINITIALIZED;
    struct local_socket* s = socket->protocol_data;
    if (s->is_open)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (addr && !addrlen)
        return OBOS_STATUS_INVALID_ARGUMENT;

    size_t addr_max = addrlen ? (*addrlen)-2 : 0;
    struct open_local_socket* con = nullptr;
    
    if (!Core_EventGetState(&s->serv.doorbell) && nonblocking)
        return OBOS_STATUS_WOULD_BLOCK;
    obos_status status = Core_WaitOnObject(WAITABLE_OBJECT(s->serv.doorbell));
    if (obos_is_error(status))
        return status;
    Core_EventClear(&s->serv.doorbell);

    Core_MutexAcquire(&s->serv.lock);
    con = LIST_GET_HEAD(open_local_socket_list, &s->serv.waiting_clients);
    LIST_REMOVE(open_local_socket_list, &s->serv.waiting_clients, con);
    Core_MutexRelease(&s->serv.lock);

    if (!con)
        return OBOS_STATUS_RETRY;

    socket_desc* res = stream_create();
    struct local_socket* scon = res->protocol_data = Vfs_Calloc(1, sizeof(struct local_socket));
    
    scon->is_open = true;
    scon->is_server = true;
    scon->open = con;

    scon->incoming_stream = &con->stream.server_bound;
    scon->outgoing_stream = &con->stream.client_bound;
    scon->peer = con->lsckt;
        
    if (addr)
    {
        if (scon->open->bound_ent)
        {
            struct sockaddr_un* laddr = (void*)addr;
            laddr->sun_family = AF_UNIX;
            char* bound_path = VfsH_DirentPath(scon->open->bound_ent, Vfs_Root);
            size_t bound_path_len = bound_path ? 0 : strlen(bound_path);
            memcpy(laddr->sun_path, bound_path, OBOS_MIN(addr_max - sizeof(laddr->sun_family), bound_path_len+1));
            *addrlen = bound_path_len;
            Vfs_Free(bound_path);
        }
        else
        {
            addr->family = AF_UNIX;
            *addrlen = 2;
        }
    }

    *out = res;

    return OBOS_STATUS_SUCCESS;
}

static obos_status stream_connect(socket_desc* socket, struct sockaddr* addr, size_t addrlen)
{
    if (socket->protocol_data)
        return OBOS_STATUS_ALREADY_INITIALIZED;

    struct sockaddr_un* uaddr = (void*)addr;
    if (!uaddr || addrlen < 2)
        return OBOS_STATUS_INVALID_ARGUMENT;
    size_t path_max = addrlen-2;
    if (!path_max || uaddr->sun_path[0] == 0)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (path_max > 108)
        return OBOS_STATUS_INVALID_ARGUMENT;

    char path[109] = {};
    memcpy(path, uaddr->sun_path, path_max);
    path[path_max] = 0;
    dirent* server_ent = VfsH_DirentLookup(path);
    if (!server_ent)
        return OBOS_STATUS_CONNECTION_REFUSED;
    vnode* server_vn = server_ent->vnode;
    OBOS_ASSERT(server_vn);
    if (server_vn->vtype != VNODE_TYPE_SOCK)
        return OBOS_STATUS_CONNECTION_REFUSED;
    // TODO: Do we need to verify if this socket is actually a UNIX socket or not?
    socket_desc* remote_socket = (void*)server_vn->desc;
    if (!remote_socket)
        return OBOS_STATUS_CONNECTION_REFUSED;
    struct local_socket *serv = remote_socket->protocol_data;
    if (!serv)    
        return OBOS_STATUS_CONNECTION_REFUSED;
    if (serv->is_open)
        return OBOS_STATUS_INVALID_OPERATION;
    
    struct local_socket* sock_data = Vfs_Calloc(1, sizeof(struct local_socket));

    sock_data->open = Vfs_Calloc(1, sizeof(struct open_local_socket));
    sock_data->is_open = true;
    sock_data->open->server = serv;
    sock_data->open->bound_ent = socket->local_ent;
    ringbuffer_initialize(&sock_data->open->stream.server_bound);
    ringbuffer_initialize(&sock_data->open->stream.client_bound);

    Core_MutexAcquire(&serv->serv.lock);
    LIST_APPEND(open_local_socket_list, &serv->serv.waiting_clients, sock_data->open);
    Core_EventSet(&serv->serv.doorbell, false);
    Core_MutexRelease(&serv->serv.lock);
    
    sock_data->incoming_stream = &sock_data->open->stream.client_bound;
    sock_data->outgoing_stream = &sock_data->open->stream.server_bound;
    sock_data->peer = sock_data->open->server;
    sock_data->open->lsckt = sock_data;

    socket->protocol_data = sock_data;

    return OBOS_STATUS_SUCCESS;
}

static obos_status stream_getpeername(socket_desc* socket, struct sockaddr* addr, size_t *addrlen)
{
    if (!addr || !addrlen)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!socket)
        return OBOS_STATUS_INVALID_ARGUMENT;
    struct local_socket* lsckt = socket->protocol_data;
    if (!lsckt)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!lsckt->is_open)
        return OBOS_STATUS_INVALID_ARGUMENT;

    dirent* peer = nullptr;
    if (lsckt->is_server)
        peer = lsckt->open->bound_ent;
    else
        peer = lsckt->open->server->serv.file;

    if (!peer)
    {
        addr->family = AF_UNIX;
        *addrlen = sizeof(addr->family);
        return OBOS_STATUS_SUCCESS;
    }

    size_t addr_max = (*addrlen) - 2;
    
    struct sockaddr_un* laddr = (void*)addr;
    laddr->sun_family = AF_UNIX;
    char* bound_path = VfsH_DirentPath(peer, Vfs_Root);
    size_t bound_path_len = bound_path ? 0 : strlen(bound_path);
    memcpy(laddr->sun_path, bound_path, OBOS_MIN(addr_max - sizeof(laddr->sun_family), bound_path_len+1));
    *addrlen = bound_path_len;
    Vfs_Free(bound_path);
    
    return OBOS_STATUS_SUCCESS;
}
static obos_status stream_getsockname(socket_desc* socket, struct sockaddr* addr, size_t *addrlen)
{
    if (!addr || !addrlen)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!socket)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!socket->local_ent)
    {
        addr->family = AF_UNIX;
        *addrlen = sizeof(addr->family);
        return OBOS_STATUS_SUCCESS;
    }

    size_t addr_max = (*addrlen) - 2;
    
    struct sockaddr_un* laddr = (void*)addr;
    laddr->sun_family = AF_UNIX;
    char* bound_path = VfsH_DirentPath(socket->local_ent, Vfs_Root);
    size_t bound_path_len = bound_path ? 0 : strlen(bound_path);
    memcpy(laddr->sun_path, bound_path, OBOS_MIN(addr_max - sizeof(laddr->sun_family), bound_path_len+1));
    *addrlen = bound_path_len;
    Vfs_Free(bound_path);
    
    return OBOS_STATUS_SUCCESS;
}

static obos_status stream_listen(socket_desc* socket, int backlog)
{
    if (!socket->local_ent)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (socket->protocol_data)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    OBOS_UNUSED(backlog);

    struct local_socket* sock_data = Vfs_Calloc(1, sizeof(struct local_socket));
    sock_data->is_server = true;
    sock_data->serv.lock = MUTEX_INITIALIZE();
    sock_data->serv.doorbell = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    socket->protocol_data = sock_data;
    sock_data->serv.file = socket->local_ent;

    return OBOS_STATUS_SUCCESS;
}

static obos_status stream_shutdown(socket_desc* desc, int how)
{
    struct local_socket* lsckt = desc->protocol_data;
    if (!lsckt)
        return OBOS_STATUS_INVALID_ARGUMENT;
    switch (how) {
        case SHUT_RD:
        {
            lsckt->incoming_stream = nullptr;
            break;
        }
        case SHUT_WR: 
        {
            lsckt->outgoing_stream = nullptr;
            break;
        }
        case SHUT_RDWR:
        {
            lsckt->incoming_stream = nullptr;
            lsckt->outgoing_stream = nullptr;
            break;
        }
        default: return OBOS_STATUS_INVALID_ARGUMENT;
    }
    return OBOS_STATUS_SUCCESS;
}

static void irp_stream_on_event_set(irp* req)
{
    socket_desc* socket = (void*)req->desc;
    struct local_socket* lsckt = socket->protocol_data;
    
    if (req->op == IRP_READ)
    {
        if (!lsckt->incoming_stream)
        {
            req->status = OBOS_STATUS_INTERNAL_ERROR;
            return;
        }
        size_t nReady = 0;
        ringbuffer_ready_count(lsckt->incoming_stream, &nReady);
        if ((nReady < req->blkCount && (req->socket_flags & MSG_WAITALL)) || !nReady)
        {
            Core_EventClear(&lsckt->incoming_stream->doorbell);
            req->status = OBOS_STATUS_IRP_RETRY;
            return;
        }
        if (req->dryOp)
        {
            req->status = OBOS_STATUS_SUCCESS;
            return;
        }
        req->status = ringbuffer_read(lsckt->incoming_stream, req->buff, OBOS_MIN(nReady, req->blkCount), &req->nBlkRead, req->socket_flags & MSG_PEEK);
    }
    else
    {
        if (!lsckt->outgoing_stream)
        {
            req->status = OBOS_STATUS_INTERNAL_ERROR;
            return;
        }
        if (req->dryOp)
        {
            req->status = OBOS_STATUS_SUCCESS;
            return;
        }
        req->status = ringbuffer_write(lsckt->outgoing_stream, req->cbuff, req->blkCount, &req->nBlkWritten);
    }
}

static obos_status stream_submit_irp(irp* req)
{
    socket_desc* socket = (void*)req->desc;
    struct local_socket* lsckt = socket->protocol_data;
    if (!lsckt)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!lsckt->is_open)
    {
        if (req->op != IRP_READ || !req->dryOp)
            return OBOS_STATUS_INVALID_ARGUMENT;
        // According to accept.2, one can use poll or select and wait
        // for a read event to be notified of incoming connections on 
        // a socket.

        req->evnt = &lsckt->serv.doorbell;
        req->on_event_set = nullptr;
        req->status = OBOS_STATUS_SUCCESS;

        return OBOS_STATUS_SUCCESS;
    }

    switch (req->op) {
        case IRP_READ: 
        {
            if (!lsckt->incoming_stream)
            {
                req->status = OBOS_STATUS_INTERNAL_ERROR;
                return OBOS_STATUS_SUCCESS;
            }
            req->evnt = &lsckt->incoming_stream->doorbell;
            break;
        }
        case IRP_WRITE: 
        {
            if (!lsckt->outgoing_stream)
            {
                req->status = OBOS_STATUS_INTERNAL_ERROR;
                return OBOS_STATUS_SUCCESS;
            }
            size_t nReady = 0;
            ringbuffer_ready_count(lsckt->outgoing_stream, &nReady);
            if (req->blkCount >= lsckt->outgoing_stream->size)
                req->blkCount = (lsckt->outgoing_stream->size - nReady);
            if ((lsckt->outgoing_stream->size - nReady) < req->blkCount)
                req->evnt = &lsckt->outgoing_stream->empty;
            else if (!req->dryOp)
                irp_stream_on_event_set(req);
            break;
        }
        default: return OBOS_STATUS_INVALID_ARGUMENT;
    }
    req->on_event_set = irp_stream_on_event_set;
    req->status = OBOS_STATUS_SUCCESS;

    return OBOS_STATUS_SUCCESS;
}