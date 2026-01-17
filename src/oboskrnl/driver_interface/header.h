/*
 * oboskrnl/driver_interface/header.h
 *
 * Copyright (c) 2024-2026 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

#include <driver_interface/pci.h>
#include <driver_interface/usb.h>

#include <scheduler/thread.h>

#include <stdarg.h>

enum { OBOS_DRIVER_MAGIC = 0x00116d868ac84e59 };
// Not required, but can speed up loading times if the driver header is put in here.
#define OBOS_DRIVER_HEADER_SECTION ".driverheader"

typedef enum driver_header_flags
{
    /// <summary>
    /// Should the driver be detected through ACPI?
    /// </summary>
    DRIVER_HEADER_FLAGS_DETECT_VIA_ACPI = 0x1,
    /// <summary>
    /// Should the driver be detected through PCI?
    /// </summary>
    DRIVER_HEADER_FLAGS_DETECT_VIA_PCI = 0x2,
    /// <summary>
    /// If the driver does not have an entry point, specify this flag.
    /// </summary>
    DRIVER_HEADER_FLAGS_NO_ENTRY = 0x4,
    /// <summary>
    /// If set, the driver chooses its entry point's stack size.
    /// Ignored if DRIVER_HEADER_FLAGS_NO_ENTRY is set.
    /// </summary>
    DRIVER_HEADER_FLAGS_REQUEST_STACK_SIZE = 0x8,
    /// <summary>
    /// Whether the driver exposes the standard driver interfaces to the kernel via the function table in the driver header.
    /// <para/>If unset, the driver needs to expose its own interfaces using DRV_EXPORT.
    /// <para/>NOTE: Every driver needs to have an ioctl callback in the function table, despite the state of this flag.
    /// </summary>
    DRIVER_HEADER_HAS_STANDARD_INTERFACES = 0x10,
    /// <summary>
    /// This flag should be set if the device is read from pipe-style.<para/>
    /// If this flag is set, any blkOffset parameter should be ignored. 
    /// </summary>
    DRIVER_HEADER_PIPE_STYLE_DEVICE = 0x20,
    /// <summary>
    /// Set if PnP should use the vendor id in the pciId field of the header.
    /// </summary>
    DRIVER_HEADER_PCI_HAS_VENDOR_ID = 0x40,
    /// <summary>
    /// Set if PnP should use the device id in the pciId field of the header.
    /// </summary>
    DRIVER_HEADER_PCI_HAS_DEVICE_ID = 0x80,
    /// <summary>
    /// Set if the driver header has the version field.
    /// </summary>
    DRIVER_HEADER_HAS_VERSION_FIELD = 0x100,
    /// <summary>
    /// Set to tell PnP to ignore the driver.
    /// </summary>
    DRIVER_HEADER_PNP_IGNORE = 0x200,
    /// <summary>
    /// Set if PnP should ignore the Prog IF in the the pciId field of the header.
    /// </summary>
    DRIVER_HEADER_PCI_IGNORE_PROG_IF = 0x400,
    /// <summary>
    /// Set if the filesystem driver wants paths for mk_file, move_desc_to, and remove_file
    /// </summary>
    DRIVER_HEADER_DIRENT_CB_PATHS = 0x800,
    /// <summary>
    /// Should the driver be detected through USB?
    /// </summary>
    DRIVER_HEADER_FLAGS_DETECT_VIA_USB = 0x1000,
} driver_header_flags;
typedef enum iterate_decision
{
    ITERATE_DECISION_CONTINUE,
    ITERATE_DECISION_STOP,
} iterate_decision;
typedef union driver_file_perm
{
    struct {
        bool other_exec : 1;
        bool other_write : 1;
        bool other_read : 1;
        bool group_exec : 1;
        bool group_write : 1;
        bool group_read : 1;
        bool owner_exec : 1;
        bool owner_write : 1;
        bool owner_read : 1;
        bool set_uid : 1;
        bool set_gid : 1;
    } OBOS_PACK;
    uint16_t mode;
} OBOS_PACK driver_file_perm;
typedef enum file_type
{
    FILE_TYPE_DIRECTORY,
    FILE_TYPE_REGULAR_FILE,
    FILE_TYPE_SYMBOLIC_LINK,
} file_type;
// Represents a driver-specific object.
// This could be a disk, partition, file, etc.
// This must be unique per-driver.
typedef uintptr_t dev_desc;

enum {
    FS_FLAGS_NOEXEC = BIT(0),
    FS_FLAGS_RDONLY = BIT(1),
};

typedef struct drv_fs_info {
    size_t fsBlockSize;
    size_t freeBlocks; // in units of 'fsBlockSize'

    size_t partBlockSize;
    size_t szFs; // in units of 'partBlockSize'

    size_t fileCount;
    size_t availableFiles; // the count of files that can be made until the partition cannot hold anymore

    size_t nameMax;

    uint32_t flags;
} drv_fs_info;

typedef struct driver_ftable
{
    // Note: If there is not an OBOS_STATUS for an error that a driver needs to return, choose the error closest to the error that you want to report,
    // or return OBOS_STATUS_INTERNAL_ERROR.

    // ---------------------------------------
    // ------- START GENERIC FUNCTIONS -------
    
    // NOTE: Every driver should have these functions implemented.

    obos_status(*get_blk_size)(dev_desc desc, size_t* blkSize);
    obos_status(*get_max_blk_count)(dev_desc desc, size_t* count);
    obos_status(*read_sync)(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead);
    obos_status(*write_sync)(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);
    obos_status(*submit_irp)(void* /* irp* */ request);
    obos_status(*finalize_irp)(void* /* irp* */ request); // optional, can be nullptr
    // Optional, can be nullptr
    // Note, *desc is subject to change by the driver
    obos_status(*reference_device)(dev_desc* desc);
    // Required, if reference_device exists. 
    obos_status(*unreference_device)(dev_desc);
    obos_status(*foreach_device)(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata), void* userdata);  // unrequired for fs drivers.
    obos_status(*query_user_readable_name)(dev_desc what, const char** name); // unrequired for fs drivers.
    // The driver dictates what the request means, and what its parameters are.
    obos_status(*ioctl)(dev_desc what, uint32_t request, void* argp);
    obos_status(*ioctl_argp_size)(uint32_t request, size_t* ret);
    // Called on driver unload.
    // Frees all the driver's allocated resources, as the kernel 
    // does not keep a track of allocated resources, and cannot free them on driver unload, causing a
    // memory leak.
    void(*driver_cleanup_callback)();

    // NOTE: These functions are optional for device drivers, and filesystem drivers shouldn't implement this.
    // Although, it is still allowed, if for any reason these functions are required for a FS driver.
    void(*on_suspend)();
    void(*on_wake)();

    // -------- END GENERIC FUNCTIONS --------
    // ---------------------------------------
    // ---------- START FS FUNCTIONS ---------

    // NOTE: FS Drivers must always return one from get_blk_size
    // get_max_blk_count is the equivalent to get_filesize
    // NOTE: Every function here must be pointing to something if the driver is an fs driver, otherwise they must be pointing to nullptr.

    // lifetime of *path is dictated by the driver.
    obos_status(*query_path)(dev_desc desc, const char** path);
    obos_status(*path_search)(dev_desc* found, void* vn, const char* what, dev_desc parent);
    obos_status(*get_linked_path)(dev_desc desc, const char** linked);
    obos_status(*vnode_search)(void** vn_found, dev_desc desc, void* dev_vn); // Not required to exist

    // vn is optional if parent is not UINTPTR_MAX (root directory).
    union {
        obos_status(*mk_file)(dev_desc* newDesc, dev_desc parent, void* vn, const char* name, file_type type, driver_file_perm perm);
        obos_status(*pmk_file)(dev_desc* newDesc, const char* parent_path, void* vn, const char* name, file_type type, driver_file_perm perm);
    };
    // If !new_parent && name, then we need to rename the file.
    // If new_parent && !name, then we need to move the file to the new parent, and keep the filename.
    // If new_parent && name, then we need to move the file to new parent and rename the file.
    union {
        obos_status(*move_desc_to)(dev_desc desc, dev_desc new_parent_desc, const char* name);
        obos_status(*pmove_desc_to)(void* vn, const char* path, const char* newpath, const char* name);
    };
    union {
        // Really just unlinks the file.
        obos_status(*remove_file)(dev_desc desc);
        obos_status(*premove_file)(void* vn, const char* path);
    };
    obos_status(*trunc_file)(dev_desc desc, size_t newsize /* note, newsize must be less than the filesize */);

    // hard links the file 'desc' to 'parent/name'
    union {
        obos_status(*hardlink_file)(dev_desc desc, dev_desc parent, const char* name);
        obos_status(*phardlink_file)(dev_desc desc, const char* parent_path, void* vn, const char* name);
    };
    obos_status(*symlink_set_path)(dev_desc desc, const char* to);

    // times is of type 'struct file_times' defined in vfs/vnode.h
    obos_status(*set_file_times)(dev_desc desc, void* times);
    obos_status(*get_file_perms)(dev_desc desc, driver_file_perm *perm);
    obos_status(*set_file_perms)(dev_desc desc, driver_file_perm newperm);
    // if an ID is -1, it means leave that field UNCHANGED
    obos_status(*set_file_owner)(dev_desc desc, uid owner_uid, gid group_uid);
    obos_status(*get_file_type)(dev_desc desc, file_type *type);
    obos_status(*get_file_inode)(dev_desc desc, uint32_t *ino);

    // If dir is UINTPTR_MAX, it refers to the root directory.
    obos_status(*list_dir)(dev_desc dir, void* vn, iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata, const char* name), void* userdata);
    obos_status(*stat_fs_info)(void *vn, drv_fs_info *info);

    obos_status(*on_usb_attach)(usb_dev_desc* desc);
    obos_status(*on_usb_detach)(usb_dev_desc* desc);

    // Can only be nullptr for the InitRD driver.
    // MUST be called before any operations on the filesystem for that vnode (e.g., list_dir, path_search).
    bool(*probe)(void* vn);
    // vn: vnode*
    // target: dirent*
    obos_status(*mount)(void* vn, void* target);

    // ----------- END FS FUNCTIONS ----------
    // ---------------------------------------
} driver_ftable;

#define CURRENT_DRIVER_HEADER_VERSION (2)
typedef struct driver_header
{
    // Set to OBOS_DRIVER_MAGIC.
    uint64_t magic;
    // See driver_header_flags
    uint32_t flags;

    // The PCI device associcated with this.
    pci_hid pciId;

    struct
    {
        // These strings are not null-terminated.
        // The PnP IDs for the driver.
        // Each one of these is first compared with the HID.
        // Then, each one of these is compared with the CID.
        char pnpIds[32][8];
        // Ranges from 1-32 inclusive.
        size_t nPnpIds;
    } acpiId;

    size_t stackSize; // If DRIVER_HEADER_FLAGS_REQUEST_STACK_SIZE is set.
    driver_ftable ftable;
    char driverName[64];

    uint32_t version;

    // If UACPI_INIT_LEVEL_EARLY, this field does nothing.
    // If a uacpi symbol is used in the driver, and this field is specified, the kernel will the current uacpi init level against this.
    // If the init level is < level, then the driver load is failed.
    // Only valid if version >= 1, and the version field exists (flags & DRIVER_HEADER_HAS_VERSION_FIELD).
    uint32_t uacpi_init_level_required;

    thread_affinity mainThreadAffinity;

    // The USB HID associated with this driver.
    usb_hid usbHid;

    // Reserved for future use
    char reserved[0x100-0x14];
} driver_header;

#define OBOS_DRIVER_HEADER_USB_HID_VALID(header) ((header)->version >= 2)

typedef struct driver_header_node
{
    struct driver_header_node *next, *prev;
    driver_header* data;
} driver_header_node;
typedef struct driver_header_list
{
    struct driver_header_node *head, *tail;
    size_t nNodes;
} driver_header_list;
#define APPEND_DRIVER_HEADER_NODE(list, node) do {\
	(node)->next = nullptr;\
	(node)->prev = nullptr;\
	if ((list).tail)\
		(list).tail->next = (node);\
	if (!(list).head)\
		(list).head = (node);\
	(node)->prev = ((list).tail);\
	(list).tail = (node);\
	(list).nNodes++;\
} while(0)
#define REMOVE_DRIVER_HEADER_NODE(list, node) do {\
	if ((list).tail == (node))\
		(list).tail = (node)->prev;\
	if ((list).head == (node))\
		(list).head = (node)->next;\
	if ((node)->prev)\
		(node)->prev->next = (node)->next;\
	if ((node)->next)\
		(node)->next->prev = (node)->prev;\
	(list).nNodes--;\
} while(0)
