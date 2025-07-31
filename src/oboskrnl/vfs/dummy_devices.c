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

static uintptr_t mt_random(void)
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
static obos_status ioctl_fb0(uint32_t request, void* argp)
{
    obos_status status = OBOS_STATUS_INVALID_IOCTL;
    if (request == 1)
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
    }
    return status;
}
static obos_status ioctl(dev_desc what, uint32_t request, void* argp) 
{
    if (what == DUMMY_FB0)
        return ioctl_fb0(request, argp);
    return OBOS_STATUS_INVALID_IOCTL; 
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
            .driver_cleanup_callback = driver_cleanup_callback,
        },
        .driverName = "Dummy Device Driver"
    }
};
vdev OBOS_DummyDriverVdev = {
    .driver = &OBOS_DummyDriver,
};

static void init_desc(dev_desc desc)
{
    if (desc == DUMMY_FB0 && !OBOS_TextRendererState.fb.base)
        return;

    dirent* ent = Vfs_Calloc(1, sizeof(dirent));
    vnode* vn = Vfs_Calloc(1, sizeof(vnode));

    vn->owner_uid = 0;
    vn->group_uid = 0;
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
            RB_INSERT(pagecache_tree, &Mm_Pagecache, pg);
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