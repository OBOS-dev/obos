/*
 * oboskrnl/vfs/dummy_devices.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <text.h>
#include <error.h>
#include <memmanip.h>
#include <klog.h>

#include <mm/context.h>
#include <mm/page.h>

#include <driver_interface/header.h>

#include <vfs/dirent.h>
#include <vfs/vnode.h>
#include <vfs/alloc.h>
#include <vfs/mount.h>

#include <irq/timer.h>

// shamelessly stolen from https://osdev.wiki/wiki/Random_Number_Generator#Mersenne_Twister

#if OBOS_ARCHITECTURE_BITS == 32
#define STATE_SIZE  624
#define MIDDLE      397
#define INIT_SHIFT  30
#define INIT_FACT   1812433253
#define TWIST_MASK  0x9908b0df
#define SHIFT1      11
#define MASK1       0xffffffff
#define SHIFT2      7
#define MASK2       0x9d2c5680
#define SHIFT3      15
#define MASK3       0xefc60000
#define SHIFT4      18
#else
#define STATE_SIZE  312
#define MIDDLE      156
#define INIT_SHIFT  62
#define TWIST_MASK  0xb5026f5aa96619e9
#define INIT_FACT   6364136223846793005
#define SHIFT1      29
#define MASK1       0x5555555555555555
#define SHIFT2      17
#define MASK2       0x71d67fffeda60000
#define SHIFT3      37
#define MASK3       0xfff7eee000000000
#define SHIFT4      43
#endif

#define LOWER_MASK  0x7fffffff
#define UPPER_MASK  (~(uintptr_t)LOWER_MASK)
static uintptr_t state[STATE_SIZE];
static size_t index = STATE_SIZE + 1;

static void mt_seed(uintptr_t s)
{
    index = STATE_SIZE;
    state[0] = s;
    for (size_t i = 1; i < STATE_SIZE; i++)
        state[i] = (INIT_FACT * (state[i - 1] ^ (state[i - 1] >> INIT_SHIFT))) + i;
}

static void twist(void)
{
    for (size_t i = 0; i < STATE_SIZE; i++)
    {
        uintptr_t x = (state[i] & UPPER_MASK) | (state[(i + 1) % STATE_SIZE] & LOWER_MASK);
        x = (x >> 1) ^ (x & 1? TWIST_MASK : 0);
        state[i] = state[(i + MIDDLE) % STATE_SIZE] ^ x;
    }
    index = 0;
}

uintptr_t mt_random(void)
{
    if (index >= STATE_SIZE)
    {
        OBOS_ASSERT(index == STATE_SIZE || !"Generator never seeded");
        twist();
    }

    uintptr_t y = state[index];
    y ^= (y >> SHIFT1) & MASK1;
    y ^= (y << SHIFT2) & MASK2;
    y ^= (y << SHIFT3) & MASK3;
    y ^= y >> SHIFT4;

    index++;
    return y;
}

static uintptr_t random_seed()
{
    return CoreS_GetNativeTimerTick();
}

enum {
    // /dev/null
    DUMMY_NULL = 1,
    // /dev/full
    DUMMY_FULL,
    // /dev/zero
    DUMMY_ZERO,
    // /dev/fb0
    DUMMY_FB0,
    // /dev/random
    DUMMY_RANDOM,
    DUMMY_MAX = DUMMY_RANDOM,
};
static const char* names[DUMMY_MAX+1] = {
    nullptr,
    "null", "full", "zero", "fb0", "random"
};

static obos_status get_blk_size(dev_desc desc, size_t* blkSize)
{
    if (!blkSize || desc < 1 || desc > DUMMY_MAX)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *blkSize = 1;
    return OBOS_STATUS_SUCCESS;
}

static obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    if (!count)
        return OBOS_STATUS_INVALID_ARGUMENT;
    switch (desc) {
        case DUMMY_NULL:
        case DUMMY_FULL:
        case DUMMY_ZERO:
            *count = 0;
            break;
        case DUMMY_FB0:
            *count = OBOS_TextRendererState.fb.pitch*OBOS_TextRendererState.fb.height;
            break;
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    return OBOS_STATUS_SUCCESS;
}

static obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    OBOS_UNUSED(blkOffset);
    if (!buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    switch (desc) {
        case DUMMY_NULL:
            *nBlkRead = 0;
            break;
        case DUMMY_FULL:
        case DUMMY_ZERO:
            memzero(buf, blkCount);
            if (nBlkRead)
                *nBlkRead = blkCount;
            break;
        case DUMMY_RANDOM:
            for (size_t i = 0; i  < blkCount / sizeof(mt_random()); i++)
                ((uintptr_t*)buf)[i] = mt_random();
            if (nBlkRead)
                *nBlkRead = blkCount;
            break;
        case DUMMY_FB0:
            return OBOS_STATUS_INVALID_OPERATION;
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    return OBOS_STATUS_SUCCESS;
}

static obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    OBOS_UNUSED(blkOffset);
    if (!buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    switch (desc) {
        case DUMMY_NULL:
        case DUMMY_ZERO:
            *nBlkWritten = blkCount;
            break;
        case DUMMY_FULL:
            return OBOS_STATUS_NOT_ENOUGH_MEMORY;
        case DUMMY_FB0:
            return OBOS_STATUS_INVALID_OPERATION;
        case DUMMY_RANDOM:
            return OBOS_STATUS_INVALID_OPERATION;
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    return OBOS_STATUS_SUCCESS;
}

static void driver_cleanup_callback(){};
struct fb_mode {
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint16_t format; // See OBOS_FB_FORMAT_*
    uint8_t bpp; 
};

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_FSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP     0x4604
#define FBIOPUTCMAP     0x4605

struct fb_fix_screeninfo {
    char id[16];            /* identification string eg "TT Builtin" */
    unsigned long smem_start;   /* Start of frame buffer mem */
                    /* (physical address) */
    uint32_t smem_len;         /* Length of frame buffer mem */
    uint32_t type;         /* see FB_TYPE_*        */
    uint32_t type_aux;         /* Interleave for interleaved Planes */
    uint32_t visual;           /* see FB_VISUAL_*      */ 
    uint16_t xpanstep;         /* zero if no hardware panning  */
    uint16_t ypanstep;         /* zero if no hardware panning  */
    uint16_t ywrapstep;        /* zero if no hardware ywrap    */
    uint32_t line_length;      /* length of a line in bytes    */
    unsigned long mmio_start;   /* Start of Memory Mapped I/O   */
                    /* (physical address) */
    uint32_t mmio_len;         /* Length of Memory Mapped I/O  */
    uint32_t accel;            /* Indicate to driver which */
                    /*  specific chip/card we have  */
    uint16_t capabilities;     /* see FB_CAP_*         */
    uint16_t reserved[2];      /* Reserved for future compatibility */
};

struct fb_bitfield {
    uint32_t offset;           /* beginning of bitfield    */
    uint32_t length;           /* length of bitfield       */
    uint32_t msb_right;        /* != 0 : Most significant bit is */ 
                    /* right */ 
};

struct fb_var_screeninfo {
    uint32_t xres;         /* visible resolution       */
    uint32_t yres;
    uint32_t xres_virtual;     /* virtual resolution       */
    uint32_t yres_virtual;
    uint32_t xoffset;          /* offset from virtual to visible */
    uint32_t yoffset;          /* resolution           */

    uint32_t bits_per_pixel;       /* guess what           */
    uint32_t grayscale;        /* 0 = color, 1 = grayscale,    */
                    /* >1 = FOURCC          */
    struct fb_bitfield red;     /* bitfield in fb mem if true color, */
    struct fb_bitfield green;   /* else only length is significant */
    struct fb_bitfield blue;
    struct fb_bitfield transp;  /* transparency         */  

    uint32_t nonstd;           /* != 0 Non standard pixel format */

    uint32_t activate;         /* see FB_ACTIVATE_*        */

    uint32_t height;           /* height of picture in mm    */
    uint32_t width;            /* width of picture in mm     */

    uint32_t accel_flags;      /* (OBSOLETE) see fb_info.flags */

    /* Timing: All values in pixclocks, except pixclock (of course) */
    uint32_t pixclock;         /* pixel clock in ps (pico seconds) */
    uint32_t left_margin;      /* time from sync to picture    */
    uint32_t right_margin;     /* time from picture to sync    */
    uint32_t upper_margin;     /* time from sync to picture    */
    uint32_t lower_margin;
    uint32_t hsync_len;        /* length of horizontal sync    */
    uint32_t vsync_len;        /* length of vertical sync  */
    uint32_t sync;         /* see FB_SYNC_*        */
    uint32_t vmode;            /* see FB_VMODE_*       */
    uint32_t rotate;           /* angle we rotate counter clockwise */
    uint32_t colorspace;       /* colorspace for FOURCC-based modes */
    uint32_t reserved[4];      /* Reserved for future compatibility */
};


static obos_status ioctl_fb0(uint32_t request, void* argp)
{
    obos_status status = OBOS_STATUS_INVALID_IOCTL;
    switch (request) {
        case 1:       
        {
            status = OBOS_STATUS_SUCCESS;
            static struct fb_mode mode = {};
            if (!mode.bpp)
            {
                mode.bpp = OBOS_TextRendererState.fb.bpp;
                mode.height = OBOS_TextRendererState.fb.height;
                mode.width = OBOS_TextRendererState.fb.width;
                mode.pitch = OBOS_TextRendererState.fb.pitch;
                mode.format = OBOS_TextRendererState.fb.format;
            }
            memcpy(argp, &mode, sizeof(mode));
            break;
        }
        case FBIOGET_FSCREENINFO:
        {
            status = OBOS_STATUS_SUCCESS;
            static struct fb_fix_screeninfo res = {};
            if (res.visual != 2)
            {
                res.visual = 2; // FB_VISUAL_TRUECOLOR
                memcpy(res.id, "Builtin OBOS FB", 16);
                res.type = 0; // FB_TYPE_PACKED_PIXELS
                res.line_length = OBOS_TextRendererState.fb.pitch;
                res.smem_len = OBOS_TextRendererState.fb.pitch*OBOS_TextRendererState.fb.height;
            }
            memcpy(argp, &res, sizeof(res));
            break;
        }
        // we're not implementing this.
        case FBIOPUT_FSCREENINFO: status = OBOS_STATUS_SUCCESS; break;
        case FBIOPUTCMAP: status = OBOS_STATUS_SUCCESS; break;
        case FBIOGET_VSCREENINFO:
        {
            status = OBOS_STATUS_SUCCESS;
            static struct fb_var_screeninfo res = {};
            if (!res.bits_per_pixel)
            {
                res.bits_per_pixel = OBOS_TextRendererState.fb.bpp;
                res.xres = res.xres_virtual = OBOS_TextRendererState.fb.width;
                res.yres = res.yres_virtual = OBOS_TextRendererState.fb.height;
                res.xoffset = res.xres_virtual-res.xres;
                res.yoffset = res.yres_virtual-res.yres;
                res.red.length = 8;
                res.green.length = 8;
                res.blue.length = 8;
                res.transp.length = 8;
                switch (OBOS_TextRendererState.fb.format) {
                    case OBOS_FB_FORMAT_BGR888:
                        res.red.offset = 16;
                        res.green.offset = 8;
                        res.blue.offset = 0;
                        res.transp.length = 0;
                        break;
                    case OBOS_FB_FORMAT_RGB888:
                        res.red.offset = 0;
                        res.green.offset = 8;
                        res.blue.offset = 16;
                        res.transp.length = 0;
                        break;
                    case OBOS_FB_FORMAT_RGBX8888:
                        res.red.offset = 8;
                        res.green.offset = 16;
                        res.blue.offset = 24;
                        res.transp.offset = 0;
                        break;
                    case OBOS_FB_FORMAT_XRGB8888:
                        res.red.offset = 0;
                        res.green.offset = 8;
                        res.blue.offset = 16;
                        res.transp.offset = 24;
                        break;
                }
                // ignore timing stuff
            }
            memcpy(argp, &res, sizeof(res));
            break;
        }
    }
    return status;
}
static obos_status ioctl(dev_desc what, uint32_t request, void* argp) 
{
    if (what == DUMMY_FB0)
        return ioctl_fb0(request, argp);
    return OBOS_STATUS_INVALID_IOCTL; 
}
static obos_status ioctl_argp_size(uint32_t request, size_t* ret)
{
    if (request == 1)
        *ret = sizeof(struct fb_mode);
    else if (request == FBIOGET_FSCREENINFO)
        *ret = sizeof(struct fb_fix_screeninfo);
    else if (request == FBIOPUT_FSCREENINFO)
        *ret = sizeof(struct fb_fix_screeninfo);
    else if (request == FBIOGET_VSCREENINFO)
        *ret = sizeof(struct fb_var_screeninfo);
    else if (request == FBIOPUTCMAP)
        *ret = 0;
    else
        return OBOS_STATUS_INVALID_IOCTL;
    return OBOS_STATUS_SUCCESS;
}

driver_id OBOS_DummyDriver = {
    .id=0,
    .header = {
        .magic = OBOS_DRIVER_MAGIC,
        .flags = DRIVER_HEADER_FLAGS_NO_ENTRY|DRIVER_HEADER_HAS_VERSION_FIELD|DRIVER_HEADER_HAS_STANDARD_INTERFACES,
        .ftable = {
            .get_blk_size = get_blk_size,
            .get_max_blk_count = get_max_blk_count,
            .write_sync = write_sync,
            .read_sync = read_sync,
            .ioctl = ioctl,
            .ioctl_argp_size = ioctl_argp_size,
            .driver_cleanup_callback = driver_cleanup_callback,
        },
        .driverName = "Dummy Device Driver"
    }
};
vdev OBOS_DummyDriverVdev = {
    .driver = &OBOS_DummyDriver,
};

#ifdef __x86_64__
#   include <arch/x86_64/cmos.h>
#endif

static long get_current_time()
{
    long current_time = 0;
#ifdef __x86_64__
    Arch_CMOSGetEpochTime(&current_time);
#endif
    return current_time;
}


static void init_desc(dev_desc desc)
{
    if (desc == DUMMY_FB0 && !OBOS_TextRendererState.fb.base)
        return;

    dirent* ent = Vfs_Calloc(1, sizeof(dirent));
    vnode* vn = Vfs_Calloc(1, sizeof(vnode));

    vn->uid = 0;
    vn->gid = 0;
    vn->desc = desc;
    get_max_blk_count(desc, &vn->filesize);

    vn->perm.owner_exec=false;
    vn->perm.group_exec=false;
    vn->perm.other_exec=false;

    vn->perm.owner_read=true;
    vn->perm.group_read=true;
    vn->perm.other_read=true;

    vn->perm.owner_write=true;
    vn->perm.group_write=true;
    vn->perm.other_write=true;

    vn->times.birth = get_current_time();
    vn->times.change = vn->times.birth;
    vn->times.access = vn->times.birth;

    vn->vtype = VNODE_TYPE_CHR;
    vn->un.device = &OBOS_DummyDriverVdev;
    ent->vnode = vn;
    vn->refs++;
    OBOS_InitString(&ent->name, names[vn->desc]);

    if (desc == DUMMY_FB0)
    {
        vn->vtype = VNODE_TYPE_BLK;
        vn->flags |= VFLAGS_FB;
        // Create pagecache entries.
        size_t fb_size = OBOS_TextRendererState.fb.pitch*OBOS_TextRendererState.fb.height;
        for (size_t i = 0; i < fb_size; i += OBOS_PAGE_SIZE)
        {
            page_info info = {};
            MmS_QueryPageInfo(Mm_KernelContext.pt, (uintptr_t)OBOS_TextRendererState.fb.base+i, &info, nullptr);
            uintptr_t phys = info.phys;
            if (info.prot.huge_page)
                phys += (i%OBOS_HUGE_PAGE_SIZE);
            page *pg = MmH_AllocatePage(phys, false);
            pg->flags |= PHYS_PAGE_MMIO;
            pg->backing_vn = vn;
            pg->file_offset = i;
            RB_INSERT(pagecache_tree, &vn->cache, pg);
        }
    }

    dirent* parent = Vfs_DevRoot;
    vn->mount_point = parent->vnode->mount_point;
    VfsH_DirentAppendChild(parent, ent);
}

void Vfs_InitDummyDevices()
{
    init_desc(DUMMY_NULL);
    init_desc(DUMMY_FULL);
    init_desc(DUMMY_ZERO);
    init_desc(DUMMY_FB0);
    mt_seed(random_seed());
    init_desc(DUMMY_RANDOM);
}

OBOS_EXPORT const char* OBOS_ScancodeToString[84] = {
    "SCANCODE_UNKNOWN",
    "SCANCODE_A",
    "SCANCODE_B",
    "SCANCODE_C",
    "SCANCODE_D",
    "SCANCODE_E",
    "SCANCODE_F",
    "SCANCODE_G",
    "SCANCODE_H",
    "SCANCODE_I",
    "SCANCODE_J",
    "SCANCODE_K",
    "SCANCODE_L",
    "SCANCODE_M",
    "SCANCODE_N",
    "SCANCODE_O",
    "SCANCODE_P",
    "SCANCODE_Q",
    "SCANCODE_R",
    "SCANCODE_S",
    "SCANCODE_T",
    "SCANCODE_U",
    "SCANCODE_V",
    "SCANCODE_W",
    "SCANCODE_X",
    "SCANCODE_Y",
    "SCANCODE_Z",
    "SCANCODE_0",
    "SCANCODE_1",
    "SCANCODE_2",
    "SCANCODE_3",
    "SCANCODE_4",
    "SCANCODE_5",
    "SCANCODE_6",
    "SCANCODE_7",
    "SCANCODE_8",
    "SCANCODE_9",
    "SCANCODE_PLUS",
    "SCANCODE_FORWARD_SLASH",
    "SCANCODE_BACKSLASH",
    "SCANCODE_STAR",
    "SCANCODE_EQUAL",
    "SCANCODE_DASH",
    "SCANCODE_UNDERSCORE",
    "SCANCODE_BACKTICK",
    "SCANCODE_QUOTATION_MARK",
    "SCANCODE_APOSTROPHE",
    "SCANCODE_SQUARE_BRACKET_LEFT",
    "SCANCODE_SQUARE_BRACKET_RIGHT",
    "SCANCODE_TAB",
    "SCANCODE_ESC",
    "SCANCODE_PGUP",
    "SCANCODE_PGDOWN",
    "SCANCODE_HOME",
    "SCANCODE_END",
    "SCANCODE_DELETE",
    "SCANCODE_BACKSPACE",
    "SCANCODE_SPACE",
    "SCANCODE_INSERT",
    "SCANCODE_F1",
    "SCANCODE_F2",
    "SCANCODE_F3",
    "SCANCODE_F4",
    "SCANCODE_F5",
    "SCANCODE_F6",
    "SCANCODE_F7",
    "SCANCODE_F8",
    "SCANCODE_F9",
    "SCANCODE_F10",
    "SCANCODE_F11",
    "SCANCODE_F12",
    "SCANCODE_DOT",
    "SCANCODE_COMMA",
    "SCANCODE_SEMICOLON",
    "SCANCODE_UP_ARROW",
    "SCANCODE_DOWN_ARROW",
    "SCANCODE_RIGHT_ARROW",
    "SCANCODE_LEFT_ARROW",
    "SCANCODE_ENTER",
    "SCANCODE_SUPER_KEY",
    "SCANCODE_CTRL",
    "SCANCODE_ALT",
    "SCANCODE_FN",
    "SCANCODE_SHIFT",
};