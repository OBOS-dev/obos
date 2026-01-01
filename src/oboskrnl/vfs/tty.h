/*
 * oboskrnl/vfs/tty.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <text.h>
#include <error.h>
#include <flanterm.h>

#include <locks/event.h>

#include <vfs/keycode.h>
#include <vfs/vnode.h>
#include <vfs/dirent.h>

// Raw TTY I/O commands
typedef struct tty_interface {
    void *userdata;
    // 'buf' is assumed to be 8 bytes.
    // if !block and there is no data to be read, return OBOS_STATUS_WOULD_BLOCK, and set *nRead to zero.
    // void* tty is a struct tty *tty
    void(*set_data_ready_cb)(void* tty, void(*cb)(void* tty, const void* buf, size_t nBytesReady));
    obos_status(*write)(void* tty, const char* buf, size_t szBuf);
    // Drain output buffers, optional to implement.
    obos_status(*tcdrain)(void* tty);
    struct {
        uint16_t row, col; // characters
        uint16_t width, height; // pixels
    } size;
} tty_interface;

#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

// lflag
#define ISIG 0000001
#define ICANON 0000002
#define ECHO 0000010
#define ECHOE 0000020
#define ECHOK 0000040
#define ECHONL 0000100
#define NOFLSH 0000200
#define TOSTOP 0000400
#define IEXTEN 0100000

// oflag
#define OPOST 0000001
#define OLCUC 0000002
#define ONLCR 0000004
#define OCRNL 0000010
#define ONOCR 0000020
#define ONLRET 0000040
#define OFILL 0000100
#define OFDEL 0000200

// iflag
#define IGNBRK 0000001
#define BRKINT 0000002
#define IGNPAR 0000004
#define PARMRK 0000010
#define INPCK 0000020
#define ISTRIP 0000040
#define INLCR 0000100
#define IGNCR 0000200
#define ICRNL 0000400
#define IUCLC 0001000
#define IXON 0002000
#define IXANY 0004000
#define IXOFF 0010000
#define IMAXBEL 0020000
#define IUTF8 0040000

struct termios
{
    uint32_t iflag;
    uint32_t oflag;
    uint32_t cflag;
    uint32_t lflag;
    uint8_t line;
    uint8_t cc[32];
    // for linux compatibilty, ignored.
    uint32_t ibaud;
    // same here
    uint32_t obaud;
};

typedef struct tty {
#define TTY_MAGIC 0x63EA62F4
    event data_ready_evnt;
    tty_interface interface;
    uint32_t magic;
    vnode* vn;
    dirent* ent;
    struct termios termios;
    struct {
        char* buf;
        size_t out_ptr;
        size_t in_ptr;
        size_t size;
    } input_buffer;
    struct process_group* fg_job;
    atomic_bool paused;
    bool quoted : 1;
    bool input_enabled : 1;
} tty;

#define TTY_IOCTL_SETATTR 0x01
#define TTY_IOCTL_GETATTR 0x02
#define TTY_IOCTL_FLOW 0x03
#define TTY_IOCTL_FLUSH 0x04
#define TTY_IOCTL_DRAIN 0x05

// Makes a copy of 'i' before creating the TTY.
obos_status Vfs_RegisterTTY(const tty_interface* i, dirent** node, bool pty);

obos_status VfsH_MakeScreenTTY(tty_interface* i, vnode* keyboard, text_renderer_state* conout, struct flanterm_context* fconout);