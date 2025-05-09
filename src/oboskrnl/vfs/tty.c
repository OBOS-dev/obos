/*
 * oboskrnl/vfs/tty.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <text.h>
#include <signal.h>
#include <memmanip.h>

#include <driver_interface/driverId.h>
#include <driver_interface/header.h>

#include <scheduler/thread.h>
#include <scheduler/process.h>
#include <scheduler/schedule.h>
#include <scheduler/thread_context_info.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <locks/event.h>

#include <irq/timer.h>

#include <locks/wait.h>

#include <vfs/alloc.h>
#include <vfs/dirent.h>
#include <vfs/keycode.h>
#include <vfs/tty.h>
#include <vfs/vnode.h>
#include <vfs/fd.h>
#include <vfs/irp.h>

static obos_status tty_get_blk_size(dev_desc ign, size_t *sz)
{
    OBOS_UNUSED(ign);
    if (!sz)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *sz = 1;
    return OBOS_STATUS_SUCCESS;
}
static obos_status tty_get_max_blk_count(dev_desc ign1, size_t *ign2) 
{
    OBOS_UNUSED(ign1);
    OBOS_UNUSED(ign2);
    return OBOS_STATUS_INVALID_OPERATION;
}

static inline size_t find_eol(tty* t, volatile const char* buf)
{
    size_t i = 0;
    for (; buf[i]; i++)
    {
        if (buf[i] == '\n' ||
            buf[i] == '\r' ||
            buf[i] == t->termios.cc[VEOL] ||
            buf[i] == t->termios.cc[VEOL2])
            return i;
    }
    return SIZE_MAX;
}

static obos_status tty_read_sync(dev_desc desc, void *buf, size_t blkCount,
                                 size_t blkOffset, size_t *nBlkRead) 
{
    OBOS_UNUSED(blkOffset);

    tty *tty = (struct tty *)desc;
    if (!tty || tty->magic != TTY_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;

    if (tty->fg_job != Core_GetCurrentThread()->proc)
        return OBOS_STATUS_INTERNAL_ERROR; // EIO

    obos_status status = OBOS_STATUS_SUCCESS;
    uint8_t *out_buf = buf;
    if (tty->termios.lflag & ICANON)
    {
        // Read until the next EOL (Either '\n', '\r', VEOL, or VEOL2)
        // size_t i = 0;
        // for (; i < blkCount; i++)
        // {
        //     while (tty->input_buffer.in_ptr >= tty->input_buffer.out_ptr)
        //         OBOSS_SpinlockHint();
        //     char ch = tty->input_buffer.buf[tty->input_buffer.in_ptr++];
        //     if (ch == '\n' || ch == '\r' || ch == tty->termios.cc[VEOL] || ch == tty->termios.cc[VEOL2])
        //         break;
        //     out_buf[i] = ch;
        // }
        size_t nBytesRead = 0;
        do {
            nBytesRead = find_eol(tty, tty->input_buffer.buf + (tty->input_buffer.in_ptr % tty->input_buffer.size));
            if (nBytesRead == SIZE_MAX)
                Core_Yield();
        } while(nBytesRead == SIZE_MAX);
        nBytesRead++;
        nBytesRead = OBOS_MIN(blkCount, nBytesRead);
        memcpy(out_buf, tty->input_buffer.buf + (tty->input_buffer.in_ptr % tty->input_buffer.size), nBytesRead);
        tty->input_buffer.in_ptr += nBytesRead;
        if (nBlkRead)
            *nBlkRead = nBytesRead;
    }
    else 
    {
        // Read until blkCount or VMIN bytes are read, or until VTIME is exceeded.
        timer_tick deadline = CoreS_GetTimerTick()+CoreH_TimeFrameToTick(tty->termios.cc[VTIME]*100000);
        size_t i = 0;
        for (; i < blkCount && i < tty->termios.cc[VMIN]; i++)
        {
            while (CoreS_GetTimerTick() > deadline && (tty->input_buffer.in_ptr == tty->input_buffer.out_ptr))
                OBOSS_SpinlockHint();
            if (CoreS_GetTimerTick() > deadline)
            {
                status = OBOS_STATUS_TIMED_OUT;
                break;
            }
            out_buf[i] = tty->input_buffer.buf[(tty->input_buffer.in_ptr++ % tty->input_buffer.size)];
        }
        if (nBlkRead)
            *nBlkRead = i;
    }


    return status;
}

static char toupper(char ch) 
{
    if (ch >= 'a' && ch <= 'z')
        return (ch - 'a') + 'A';
    return ch;
}

static obos_status tty_write_sync(dev_desc desc, const void *buf,
                                  size_t blkCount, size_t blkOffset,
                                  size_t *nBlkWritten) {
    OBOS_UNUSED(blkOffset);

    tty *tty = (struct tty *)desc;
    if (!tty || tty->magic != TTY_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    obos_status status = OBOS_STATUS_SUCCESS;
    if (!tty->termios.oflag)
        status = tty->interface.write(tty, buf, blkCount);
    else 
    {
        const char *str = buf;
        for (size_t i = 0; i < blkCount; i++) {
        switch (str[i]) {
            case '\n': {
                if (tty->termios.oflag & ONLCR)
                    status = tty->interface.write(tty, "\r\n", 2);
                else if (tty->termios.oflag & ONLRET)
                    ; // TODO: Implement.
                else
                    status = tty->interface.write(tty, "\n", 1);
                break;
            }
            case '\r': {
                if (tty->termios.oflag & OCRNL)
                    status = tty->interface.write(tty, "\n", 1);
                else
                    status = tty->interface.write(tty, "\r", 1);
                break;
            }
            default:
                if (tty->termios.oflag & OLCUC) 
                {
                    char ch = toupper(str[i]);
                    status = tty->interface.write(tty, &ch, 1);
                    break;
                }
                status = tty->interface.write(tty, str, 1);
                break;
            }
            if (obos_is_error(status)) 
            {
                if (nBlkWritten)
                    *nBlkWritten = i;
                return status;
            }
        }
    }
    if (nBlkWritten)
        *nBlkWritten = blkCount;
    return status;
}

#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
static obos_status tty_ioctl_argp_size(uint32_t request, size_t *ret) 
{
    if (!ret)
        return OBOS_STATUS_INVALID_ARGUMENT;
    obos_status status = OBOS_STATUS_SUCCESS;
    switch (request)
    {
        case TTY_IOCTL_SETATTR:
        case TTY_IOCTL_GETATTR:
            *ret = sizeof(struct termios);
            break;
        case TTY_IOCTL_DRAIN:
            *ret = 0;
            break;
        case TTY_IOCTL_FLOW:
            *ret = sizeof(uint32_t);
            break;
        case TTY_IOCTL_FLUSH:
            *ret = 0;
            status = OBOS_STATUS_UNIMPLEMENTED;
            break;
        case TIOCSPGRP:
        case TIOCGPGRP:
            *ret = sizeof(uint32_t);
            break;
        default: 
            *ret = 0;
            status = OBOS_STATUS_INVALID_IOCTL;
            break;
    }
    return status;
}
enum {
    TCOOFF,
    TCOON,
    TCIOFF,
    TCION,
};
static obos_status tty_ioctl(dev_desc what, uint32_t request, void *argp) 
{
    tty *tty = (struct tty *)what;
    if (!tty)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (tty->magic != TTY_MAGIC)
        return OBOS_STATUS_NOT_A_TTY;
    obos_status status = OBOS_STATUS_SUCCESS;
    switch (request) {
        case TTY_IOCTL_SETATTR:
            memcpy(&tty->termios, argp, sizeof(struct termios));
            break;
        case TTY_IOCTL_GETATTR:
            memcpy(argp, &tty->termios, sizeof(struct termios));
            break;
        case TIOCSPGRP:
        {
            uint32_t /* pid_t */ *pid = argp;
            process* proc = Core_LookupProc(*pid);
            if (!proc || *pid == 0)
                status = OBOS_STATUS_INVALID_ARGUMENT;
            else
                tty->fg_job = proc;
            break;
        }
        case TIOCGPGRP:
        {
            uint32_t /* pid_t */ *pid = argp;
            *pid = tty->fg_job->pid;
            break;
        }
        case TTY_IOCTL_FLOW:
            switch (*(uint32_t*)argp) {
                case TCOOFF:
                    tty->paused = false;
                    break;
                case TCOON:
                    tty->paused = true;
                    break;
                case TCIOFF:
                {
                    char ch = tty->termios.iflag & IXON ? tty->termios.cc[VSTOP] : '\023' /* assume it is this */;
                    tty->interface.write(tty, &ch, 1);
                    break;
                }
                case TCION:
                {
                    char ch = tty->termios.iflag & IXON ? tty->termios.cc[VSTART] : '\021' /* assume it is this */;
                    tty->interface.write(tty, &ch, 1);
                    break;
                }
                default:
                    status = OBOS_STATUS_INVALID_ARGUMENT;
                    break;
            }
            break;
        case TTY_IOCTL_FLUSH:
            status = OBOS_STATUS_UNIMPLEMENTED;
            break;
        case TTY_IOCTL_DRAIN:
            status = tty->interface.tcdrain ? tty->interface.tcdrain(tty) : OBOS_STATUS_SUCCESS;
            break;
        default:
            status = OBOS_STATUS_INVALID_IOCTL;
            break;
    }
    return status;
}

void irp_on_event_set(irp* req)
{
    OBOS_UNUSED(req);
    tty *tty = (struct tty *)req->desc;

    size_t nToRead = OBOS_MIN(tty->input_buffer.out_ptr - tty->input_buffer.in_ptr, req->blkCount);

    if (!req->dryOp)
    {
        memcpy(req->drvData, tty->input_buffer.buf + (tty->input_buffer.in_ptr % tty->input_buffer.size), nToRead);
        req->drvData = (void*)(uintptr_t)req->drvData + nToRead;
        tty->input_buffer.in_ptr += nToRead;
    }
    if (nToRead < req->blkCount && ~tty->termios.lflag & ICANON)
        req->status = OBOS_STATUS_IRP_RETRY;
    else
        req->status = OBOS_STATUS_SUCCESS;
    req->nBlkRead += nToRead;
    // Only if we have satisfied all bytes.
    if (tty->input_buffer.out_ptr <= tty->input_buffer.in_ptr && !req->dryOp)
        Core_EventClear(req->evnt);
}

static obos_status tty_submit_irp(void* request)
{
    irp* req = request;
    if (!req)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    tty *tty = (struct tty *)req->desc;
    if (!tty || tty->magic != TTY_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;

    if (req->op == IRP_WRITE)
    {
        // Writes don't need to be done asynchronously (if even possible to do without a worker thread).
        if (!req->dryOp)
            req->status = tty_write_sync(req->desc, req->cbuff, req->blkCount, req->blkOffset, &req->nBlkWritten);
        req->evnt = nullptr;
        return OBOS_STATUS_SUCCESS;
    }

    // Reads can be done asynchronously, so do that.

    if (tty->fg_job != Core_GetCurrentThread()->proc)
    {
        OBOS_Kill(Core_GetCurrentThread(), Core_GetCurrentThread(), SIGTTIN);
        if (~Core_GetCurrentThread()->signal_info->pending & BIT(SIGTTIN - 1))
            req->status = OBOS_STATUS_INTERNAL_ERROR; // EIO
        else
            req->status = OBOS_STATUS_SUCCESS; // EIO
        req->evnt = nullptr;
        return OBOS_STATUS_SUCCESS;
    }

    req->drvData = req->buff;
    req->evnt = &tty->data_ready_evnt;
    req->on_event_set = irp_on_event_set;
    return OBOS_STATUS_SUCCESS;
}

static size_t last_tty_index = 0;
static size_t last_pty_index = 0;

driver_id OBOS_TTYDriver = {
    .id = 0,
    .header = {.magic = OBOS_DRIVER_MAGIC,
               .flags = DRIVER_HEADER_FLAGS_NO_ENTRY |
                        DRIVER_HEADER_HAS_VERSION_FIELD |
                        DRIVER_HEADER_HAS_STANDARD_INTERFACES,
               .ftable =
                   {
                       .get_blk_size = tty_get_blk_size,
                       .get_max_blk_count = tty_get_max_blk_count,
                       .write_sync = tty_write_sync,
                       .read_sync = tty_read_sync,
                       .ioctl = tty_ioctl,
                       .ioctl_argp_size = tty_ioctl_argp_size,
                       .submit_irp = tty_submit_irp,
                       .finalize_irp = nullptr,
                       .driver_cleanup_callback = nullptr,
                   },
               .driverName = "TTY Driver"}};

uint8_t default_control[32] = {
    [VINTR] = '\003',    [VQUIT] = '\034', [VERASE] = '\177', [VKILL] = '\025',
    [VEOF] = '\004',     [VTIME] = '\000', [VMIN] = '\000',   [VSWTC] = '\000',
    [VSTART] = '\021', // Only recognized when IXON is set.
    [VSTOP] = '\023',  // Only recognized when IXON is set.
    [VSUSP] = '\032',    [VEOL] = '\000',
    [VREPRINT] = '\022', // Only recognized when ICANON and IEXTEN are set
    [VDISCARD] = '\017', // Only recognized when IEXTEN is set
    [VWERASE] = '\027',
    [VLNEXT] = '\017', // Only recognized when IEXTEN is set
    [VEOL2] = '\000',
};

static size_t strrfind(const char* str, char ch)
{
    int64_t i = strlen(str);
    for (; i >= 0; i--)
        if (str[i] == ch)
           return i;
    return SIZE_MAX;
}

void erase_bytes(tty* tty, size_t nBytesToErase)
{
    if (nBytesToErase == SIZE_MAX)
        nBytesToErase = tty->input_buffer.out_ptr % tty->input_buffer.size;
    for (size_t j = 0; j < nBytesToErase && tty->input_buffer.out_ptr > 0; j++)
    {
        tty->input_buffer.buf[--tty->input_buffer.out_ptr % tty->input_buffer.size] = 0;
        tty->interface.write(tty, "\b", 1);
    }
}

static void tty_kill(tty* tty, int sigval)
{
    if (tty->termios.lflag & ISIG)
        OBOS_KillProcess(tty->fg_job, sigval);
}

static void data_ready(void *tty_, const void *buf, size_t nBytesReady) {
    tty *tty = (struct tty *)tty_;
    const uint8_t *buf8 = buf;
    for (size_t i = 0; i < nBytesReady; i++) 
    {
        bool insert_byte = true;
        if (!tty->quoted) 
        {
            insert_byte = false;
            if (buf8[i] == tty->termios.cc[VLNEXT] && (tty->termios.lflag & (ICANON|IEXTEN)))
                insert_byte = !(tty->quoted = true);
            else
                tty->quoted = false;
            if (buf8[i] == tty->termios.cc[VINTR])
                tty_kill(tty, SIGINT);
            else if (buf8[i] == tty->termios.cc[VQUIT])
                tty_kill(tty, SIGQUIT);
            else if (buf8[i] == tty->termios.cc[VSUSP])
                tty_kill(tty, SIGTSTP);
            else
                insert_byte = true;
            if ((buf8[i] == tty->termios.cc[VERASE] || buf8[i] == tty->termios.cc[VWERASE]) && (tty->termios.lflag & (ICANON|ECHOE)))
            {
                size_t nBytesToErase = buf8[i] == tty->termios.cc[VERASE] ? 1 : 0;
                if (nBytesToErase == 0)
                {
                    size_t index_space = strrfind(tty->input_buffer.buf, ' ');
                    if (index_space != SIZE_MAX)
                        nBytesToErase = (tty->input_buffer.out_ptr % tty->input_buffer.size) - index_space - 1;
                    size_t last_ln = strrfind(tty->input_buffer.buf, '\n');
                    if (last_ln > index_space && last_ln != SIZE_MAX)
                        nBytesToErase = (tty->input_buffer.out_ptr % tty->input_buffer.size) - last_ln - 1;
                }
                if ((~tty->termios.lflag & IEXTEN) && buf8[i] == tty->termios.cc[VWERASE])
                    nBytesToErase = 0;
                erase_bytes(tty, nBytesToErase);
                insert_byte = false;
            }
            if (buf8[i] == tty->termios.cc[VKILL] && (tty->termios.lflag & (ICANON|ECHOK)))
            {
                size_t nBytesToErase = (tty->input_buffer.out_ptr % tty->input_buffer.size) - strrfind(tty->input_buffer.buf, '\n') - 1;
                erase_bytes(tty, nBytesToErase);
                insert_byte = false;
            }
        }
        else
            tty->quoted = false;
        if (!insert_byte)
            continue;
        if (tty->termios.lflag & ICANON)
        {
            if (tty->termios.lflag & ECHO)
                tty->interface.write(tty, (char*)&buf8[i], 1);
            else if (tty->termios.lflag & ECHONL && buf8[i] == '\n')
                tty->interface.write(tty, "\n", 1);
            
        }
        if (tty->termios.iflag & IGNCR && buf8[i] == '\r')
            continue;
        if (tty->termios.iflag & INLCR && buf8[i] == '\n')
        {
            tty->input_buffer.buf[tty->input_buffer.out_ptr++ % tty->input_buffer.size] = '\r';
            continue;
        }
        uint8_t mask = tty->termios.iflag & ISTRIP ? 0x7f : 0xff;
        tty->input_buffer.buf[tty->input_buffer.out_ptr++ % tty->input_buffer.size] = 
            (tty->termios.iflag & (IUCLC|IEXTEN|ICANON)) ? 
                toupper(buf8[i] & mask) : 
                (buf8[i] & mask);
        if (tty->termios.lflag & ICANON)
            find_eol(tty, &tty->input_buffer.buf[(tty->input_buffer.out_ptr - 1) % tty->input_buffer.size]) == SIZE_MAX ? (void)0 : Core_EventSet(&tty->data_ready_evnt, true);
        else
            Core_EventSet(&tty->data_ready_evnt, true);
    }
}

obos_status Vfs_RegisterTTY(const tty_interface *i, dirent **onode, bool pty) 
{
    pty = !!pty;
    if (!i || !i->set_data_ready_cb || !i->write)
        return OBOS_STATUS_INVALID_ARGUMENT;
    tty *tty = Vfs_Calloc(1, sizeof(struct tty));

    tty->magic = TTY_MAGIC;
    tty->paused = false;
    tty->data_ready_evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);

    tty->interface = *i;

    // Initialize the input ring buffer.
    tty->input_buffer.size = 4096;
    tty->input_buffer.buf = Vfs_Malloc(tty->input_buffer.size);
    tty->input_buffer.in_ptr = 0;
    tty->input_buffer.out_ptr = 0;

    memcpy(tty->termios.cc, default_control, sizeof(default_control));
    tty->termios.lflag = ICANON|ECHO|ECHOE|IEXTEN|ISIG;
    tty->termios.oflag = 0;
    tty->termios.iflag = 0;

    vnode *vn = Drv_AllocateVNode(&OBOS_TTYDriver, (dev_desc)tty, 0, nullptr,
                                    VNODE_TYPE_CHR);
    vn->flags |= VFLAGS_IS_TTY;
    vn->data = tty;
    size_t index = pty ? last_pty_index++ : last_tty_index++;
    size_t szName = snprintf(nullptr, 0, "tty%ld", index);
    char *name = nullptr;
    snprintf((name = Vfs_Malloc(szName + 1)), szName + 1, "tty%ld", index);
    OBOS_Log("%s: Registering TTY %s\n", __func__, name);
    dirent *node = Drv_RegisterVNode(vn, name);
    Vfs_Free(name);

    tty->ent = node;
    tty->vn = vn;
    if (onode)
        *onode = node;

    i->set_data_ready_cb(tty, data_ready);

    return OBOS_STATUS_SUCCESS;
}

struct screen_tty {
    fd keyboard;
    text_renderer_state* out;
    void(*data_ready)(void* tty, const void* buf, size_t nBytesReady);
    thread* data_ready_thread;
    tty* tty;
};

static char number_to_secondary(enum scancode code)
{
    char ch = 0;
    switch (code) {
        case SCANCODE_0:
            ch = ')';
            break;
        case SCANCODE_1:
            ch = '!';
            break;
        case SCANCODE_2:
            ch = '@';
            break;
        case SCANCODE_3:
            ch = '#';
            break;
        case SCANCODE_4:
            ch = '$';
            break;
        case SCANCODE_5:
            ch = '%';
            break;
        case SCANCODE_6:
            ch = '^';
            break;
        case SCANCODE_7:
            ch = '&';
            break;
        case SCANCODE_8:
            ch = '*';
            break;
        case SCANCODE_9:
            ch = '(';
            break;
        default: break;
    }
    return ch;
}

static void poll_keyboard(struct screen_tty* data)
{
    keycode tmp_code = 0;
    keycode* keycode_buffer = &tmp_code;
    while (1)
    {
        size_t nReady = 1;
        irp* req = VfsH_IRPAllocate();
        req->vn = data->keyboard.vn;
        req->buff = keycode_buffer;
        req->blkCount = 1;
        req->op = IRP_READ;
        req->dryOp = false;
        req->status = OBOS_STATUS_SUCCESS;
        VfsH_IRPSubmit(req, nullptr);
        VfsH_IRPWait(req);
        VfsH_IRPUnref(req);
        char* buffer = Vfs_Malloc(nReady);
        // size_t bufSize = nReady;
        for (size_t i = 0; i < nReady;)
        {
            keycode code = keycode_buffer[i];
            enum scancode scancode = SCANCODE_FROM_KEYCODE(code);
            enum modifiers mod = MODIFIERS_FROM_KEYCODE(code);
            if (mod & KEY_RELEASED)
            {
                nReady--;
                continue;
            }
            if (scancode <= SCANCODE_Z)
                buffer[i++] = (mod & CTRL) ? scancode : ((((mod & CAPS_LOCK) || (mod & SHIFT)) ? 'A' : 'a') + (scancode-SCANCODE_A));
            else
            {
                switch (scancode)
                {
                    case SCANCODE_0 ... SCANCODE_9:
                        if (mod & SHIFT)
                            buffer[i] = number_to_secondary(scancode);
                        else
                            buffer[i] = '0' + (scancode-SCANCODE_0);
                        break;
                    case SCANCODE_FORWARD_SLASH:
                        buffer[i] = (mod & NUMPAD) ? '/' : ((mod & SHIFT) ? '?' : '/');
                        break;
                    case SCANCODE_PLUS:
                        buffer[i] = '+';
                        break;
                    case SCANCODE_STAR:
                        buffer[i] = '*';
                        break;
                    case SCANCODE_ENTER:
                        buffer[i] = '\n';
                        break;
                    case SCANCODE_TAB:
                        buffer[i] = '\t';
                        break;
                    case SCANCODE_DOT:
                        buffer[i] = (mod & NUMPAD) ? '.' : ((~mod & SHIFT) ? '.' : '>');
                        break;
                    case SCANCODE_SQUARE_BRACKET_LEFT:
                        buffer[i] = (~mod & SHIFT) ? '[' : '{';
                        break;
                    case SCANCODE_SQUARE_BRACKET_RIGHT:
                        buffer[i] = (~mod & SHIFT) ? ']' : '}';
                        break;
                    case SCANCODE_SEMICOLON:
                        buffer[i] = (~mod & SHIFT) ? ';' : ':';
                        break;
                    case SCANCODE_COMMA:
                        buffer[i] = (~mod & SHIFT) ? ',' : '<';
                        break;
                    case SCANCODE_APOSTROPHE:
                        buffer[i] = (~mod & SHIFT) ? '\'' : '\"';
                        break;
                    case SCANCODE_BACKTICK:
                        buffer[i] = (~mod & SHIFT) ? '`' : '~';
                        break;
                    case SCANCODE_UNDERSCORE:
                        buffer[i] = (~mod & SHIFT) ? '_' : '-';
                        break;
                    case SCANCODE_BACKSLASH:
                        buffer[i] = (~mod & SHIFT) ? '\\' : '|';
                        break;
                    case SCANCODE_SPACE:
                        buffer[i] = ' ';
                        break;
                    case SCANCODE_EQUAL:
                        buffer[i] = (mod & SHIFT) ? '+' : '=';
                        break;
                    case SCANCODE_DASH:
                        buffer[i] = (mod & SHIFT) ? '_' :  '-';
                        break;
                    case SCANCODE_BACKSPACE:
                        buffer[i] = '\177';
                        break;
                    {
                    char ch = '\0';
                    case SCANCODE_DOWN_ARROW:
                        ch = 'B';
                        goto down;
                    case SCANCODE_UP_ARROW:
                        ch = 'A';
                        goto down;
                    case SCANCODE_RIGHT_ARROW:
                        ch = 'C';
                        goto down;
                    case SCANCODE_LEFT_ARROW:
                        ch = 'D';
                        
                        down:
                        nReady += 3;
                        char buf[4] = {"\x1f[\0"};
                        buf[3] = ch;
                        buffer = Vfs_Realloc(buffer, nReady);
                        buffer[i++] = buf[0];
                        buffer[i++] = buf[1];
                        buffer[i++] = buf[2];
                        buffer[i] = buf[3];
                        break;

                    }
                    default:
                        i--;
                        nReady--;
                        break;
                }
                i++;
            }
        }
        if (nReady)
            data->data_ready(data->tty, buffer, nReady);
        Vfs_Free(buffer);
    }
}

static void screen_set_data_ready_cb(void* tty_, void(*cb)(void* tty, const void* buf, size_t nBytesReady))
{
    tty* tty = tty_;
    struct screen_tty *data = tty->interface.userdata;
    data->data_ready = cb;
    data->tty = tty;
    if (!data->data_ready_thread)
    {
        data->data_ready_thread = CoreH_ThreadAllocate(nullptr);
        thread_ctx ctx = {};
        void* stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x4000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr);
        CoreS_SetupThreadContext(&ctx, (uintptr_t)poll_keyboard, (uintptr_t)data, false, stack, 0x4000);
        CoreH_ThreadInitialize(data->data_ready_thread, THREAD_PRIORITY_HIGH, Core_DefaultThreadAffinity, &ctx);
        CoreH_ThreadReady(data->data_ready_thread);
        Core_ProcessAppendThread(OBOS_KernelProcess, data->data_ready_thread);
    }
}

static obos_status screen_write(void* tty_, const char* buf, size_t szBuf)
{
    tty* tty = tty_;
    struct screen_tty *data = tty->interface.userdata;
    // for (size_t i = 0; i < szBuf; i++)
    //     OBOS_WriteCharacter(data->out, buf[i]);
    printf("%.*s", szBuf, buf);
    data->out->paused = tty->paused;
    if (!data->out->paused)
        OBOS_FlushBuffers(data->out);
    return OBOS_STATUS_SUCCESS;
}

obos_status VfsH_MakeScreenTTY(tty_interface* i, vnode* keyboard, text_renderer_state* conout)
{
    struct screen_tty* screen = Vfs_Calloc(1, sizeof(*screen));
    obos_status status = Vfs_FdOpenVnode(&screen->keyboard, keyboard, FD_OFLAGS_READ|FD_OFLAGS_UNCACHED);
    if (obos_is_error(status))
        return status;
    screen->out = conout;
    i->write = screen_write;
    i->set_data_ready_cb = screen_set_data_ready_cb;
    i->userdata = screen;
    return status;
}
