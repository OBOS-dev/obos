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
static obos_status ringbuffer_write(struct ringbuffer* buf, void* buffer, size_t sz, size_t *bytes_written)
{
    if (!buf || !buffer)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if ((sz+buf->ptr) >= buf->size)
        sz = buf->size - buf->ptr;
    void* out_ptr = (void*)((uintptr_t)buf->buffer + buf->ptr);    
    memcpy(out_ptr, buffer, sz);
    buf->ptr += sz;
    Core_EventSet(&buf->doorbell, false);
    if (bytes_written)
        *bytes_written = sz;
    return OBOS_STATUS_SUCCESS;
}
static obos_status ringbuffer_read(struct ringbuffer* stream, void* buffer, size_t sz, size_t* bytes_read, bool peek)
{
    if (!stream || !buffer)
        return OBOS_STATUS_INVALID_ARGUMENT;
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
    return OBOS_STATUS_SUCCESS;
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
    struct server_local_socket* server;
    dirent* bound_ent;
    int type;
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
static void stream_free(socket_desc* socket)
{
    Vfs_Free(socket->protocol_data);
    Vfs_Free(socket);
}
static obos_status stream_bind(socket_desc* socket, struct sockaddr* addr, size_t addrlen);
static obos_status stream_accept(socket_desc* socket, struct sockaddr* addr, size_t* addrlen, int flags, socket_desc** out);
static obos_status stream_connect(socket_desc* socket, struct sockaddr* addr, size_t addrlen);
static obos_status stream_getpeername(socket_desc* socket, struct sockaddr* addr, size_t *addrlen);
static obos_status stream_getsockname(socket_desc* socket, struct sockaddr* addr, size_t *addrlen);
static obos_status stream_listen(socket_desc* socket, int backlog);
static obos_status stream_shutdown(socket_desc* desc, int how);
static obos_status stream_submit_irp(irp* req);
static obos_status stream_finalize_irp(irp* req);

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
    .finalize_irp = stream_finalize_irp,
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

    dirent* parent = Core_GetCurrentThread()->proc->cwd;
    size_t index = strrfind(name, '/');
    char* dirname = name;
    if (index != SIZE_MAX)
    {
        dirname = name+index;
        *dirname = 0;
        dirname++;
        parent = VfsH_DirentLookupFrom(name, parent);
    }
    if (VfsH_DirentLookupFrom(dirname, parent))
        return OBOS_STATUS_ALREADY_INITIALIZED;

    vnode* vn = socket->vn;
    dirent* ent = Vfs_Calloc(1, sizeof(dirent));
    OBOS_InitString(&ent->name, name);
    ent->vnode = vn;
    VfsH_DirentAppendChild(parent, ent);
    socket->local_ent = ent;

    return OBOS_STATUS_SUCCESS;
}

static obos_status stream_accept(socket_desc* socket, struct sockaddr* addr, size_t* addrlen, int flags, socket_desc** out)
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

    size_t addr_max = addrlen ? *addrlen : 0;
    struct open_local_socket* con = nullptr;
    
    Core_WaitOnObject(WAITABLE_OBJECT(s->serv.doorbell));
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

    if (addr)
    {
        struct sockaddr_un* laddr = (void*)addr;
        char* bound_path = VfsH_DirentPath(scon->open->bound_ent, Vfs_Root);
        size_t bound_path_len = bound_path ? 0 : strlen(bound_path);
        memcpy(laddr->sun_path, bound_path, OBOS_MIN(addr_max - sizeof(laddr->sun_family), bound_path_len+1));
        *addrlen = bound_path_len;
        Vfs_Free(bound_path);
    }

    *out = res;

    return OBOS_STATUS_SUCCESS;
}

static obos_status stream_connect(socket_desc* socket, struct sockaddr* addr, size_t addrlen)
{
    OBOS_UNUSED(socket && addr && addrlen);
    return OBOS_STATUS_UNIMPLEMENTED;
}
static obos_status stream_getpeername(socket_desc* socket, struct sockaddr* addr, size_t *addrlen)
{
    OBOS_UNUSED(socket && addr && addrlen);
    return OBOS_STATUS_UNIMPLEMENTED;
}
static obos_status stream_getsockname(socket_desc* socket, struct sockaddr* addr, size_t *addrlen)
{
    OBOS_UNUSED(socket && addr && addrlen);
    return OBOS_STATUS_UNIMPLEMENTED;
}
static obos_status stream_listen(socket_desc* socket, int backlog)
{
    if (!socket->local_ent)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (socket->protocol_data)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    // bind() already does this
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
    OBOS_UNUSED(desc && how);
    return OBOS_STATUS_UNIMPLEMENTED;
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
        req->status = OBOS_STATUS_SUCCESS;

        return OBOS_STATUS_SUCCESS;
    }
    return OBOS_STATUS_UNIMPLEMENTED;;
}

static obos_status stream_finalize_irp(irp* req)
{
    OBOS_UNUSED(req);
    return OBOS_STATUS_SUCCESS;
}