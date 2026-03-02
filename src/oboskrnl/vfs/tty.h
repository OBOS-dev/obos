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
    void(*on_termios_set)(void* tty, void* new_termios, void* old_termios);
    void(*ref)(void* tty);
    void(*deref)(void* tty);
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

// cflag
#define CBAUD 0010017
#define CSIZE 0000060
#define CS5 0000000
#define CS6 0000020
#define CS7 0000040
#define CS8 0000060
#define CSTOPB 0000100
#define CREAD 0000200
#define PARENB 0000400
#define PARODD 0001000
#define HUPCL 0002000
#define CLOCAL 0004000

// speed_t constants
#define B0       0
#define B50      1
#define B75      2
#define B110     3
#define B134     4
#define B150     5
#define B200     6
#define B300     7
#define B600     8
#define B1200    9
#define B1800    10
#define B2400    11
#define B4800    12
#define B9600    13
#define B19200   14
#define B38400   15
#define B57600   0010001
#define B115200  0010002
#define B230400  0010003
#define B460800  0010004
#define B500000  0010005
#define B576000  0010006
#define B921600  0010007
#define B1000000 0010010
#define B1152000 0010011
#define B1500000 0010012
#define B2000000 0010013
#define B2500000 0010014
#define B3000000 0010015
#define B3500000 0010016
#define B4000000 0010017

static inline uint32_t speed_t_to_baud_rate(uint16_t speed)
{
    switch (speed) {
        case B0: return 0;
        case B50: return 50;
        case B75: return 75;
        case B110: return 110;
        case B134: return 134;
        case B150: return 150;
        case B200: return 200;
        case B300: return 300;
        case B600: return 600;
        case B1200: return 1200;
        case B1800: return 1800;
        case B2400: return 2400;
        case B4800: return 4800;
        case B9600: return 9600;
        case B19200: return 19200;
        case B38400: return 38400;
        case B57600: return 57600;
        case B115200: return 115200;
        case B230400: return 230400;
        case B460800: return 460800;
        case B500000: return 500000;
        case B576000: return 576000;
        case B921600: return 921600;
        case B1000000: return 1000000;
        case B1152000: return 1152000;
        case B1500000: return 1500000;
        case B2000000: return 2000000;
        case B2500000: return 2500000;
        case B3000000: return 3000000;
        case B3500000: return 3500000;
        case B4000000: return 4000000;
        default: return UINT16_MAX;
    }
}

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
    struct session* session;
    atomic_bool paused;
    bool quoted : 1;
    bool pty : 1;
    bool input_enabled : 1;
    bool hang : 1;
} tty;

#define TTY_IOCTL_SETATTR 0x01
#define TTY_IOCTL_GETATTR 0x02
#define TTY_IOCTL_FLOW 0x03
#define TTY_IOCTL_FLUSH 0x04
#define TTY_IOCTL_DRAIN 0x05

enum {
    TTY_SCREEN,
    TTY_SERIAL,
    TTY_PSUEDO,
};

// Makes a copy of 'i' before creating the TTY.
OBOS_EXPORT obos_status Vfs_RegisterTTY(const tty_interface* i, dirent** node, int type);
OBOS_EXPORT obos_status Vfs_FreeTTY(tty* tty);
OBOS_EXPORT obos_status Vfs_TTYHangUp(tty* tty);

obos_status VfsH_MakeScreenTTY(tty_interface* i, vnode* keyboard, text_renderer_state* conout, struct flanterm_context* fconout);

obos_status VfsH_MakePTM(dev_desc* ptm);
obos_status VfsH_GetPTS(dev_desc ptm, dirent** pts);
obos_status VfsH_SetPTS(dev_desc ptm, dirent* node, int idx);

extern tty_interface Vfs_PTSInterface;

// Creates /dev/ptmx
obos_status Vfs_CreatePTMX();