/*
 * src/uHDA/hdadev.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <handle.h>
#include <klog.h>
#include <memmanip.h>

#include <uhda/uhda.h>
#include <uhda/types.h>

#include <vfs/alloc.h>
#include <vfs/dirent.h>
#include <vfs/vnode.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <driver_interface/driverId.h>

#include <irq/dpc.h>

#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>
#include <scheduler/process.h>

#include <vfs/vnode.h>

OBOS_WEAK obos_status hda_get_blk_size(dev_desc desc, size_t* blkSize)
{
    OBOS_UNUSED(desc);
    *blkSize = 1;
    return OBOS_STATUS_SUCCESS;
}
obos_status hda_get_max_blk_count(dev_desc desc, size_t* count)
{
    OBOS_UNUSED(count && desc);
    return OBOS_STATUS_INVALID_OPERATION;
}
obos_status hda_read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    return OBOS_STATUS_UNIMPLEMENTED;
}
obos_status hda_write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);

void driver_cleanup_callback(){};
OBOS_WEAK obos_status ioctl(dev_desc what, uint32_t request, void* argp);
OBOS_WEAK obos_status ioctl_argp_size(uint32_t request, size_t* osize);

static driver_id HDADriver = {
    .id=0,
    .header = {
        .magic = OBOS_DRIVER_MAGIC,
        .flags = DRIVER_HEADER_FLAGS_NO_ENTRY|DRIVER_HEADER_HAS_VERSION_FIELD|DRIVER_HEADER_HAS_STANDARD_INTERFACES,
        .ftable = {
            .get_blk_size = hda_get_blk_size,
            .get_max_blk_count = hda_get_max_blk_count,
            .write_sync = hda_write_sync,
            .read_sync = hda_read_sync,
            .ioctl = ioctl,
            .ioctl_argp_size = ioctl_argp_size,
            .driver_cleanup_callback = driver_cleanup_callback,
        },
        .driverName = "uHDA Device Interface"
    }
};
static vdev HDAVdev = {
    .driver = &HDADriver,
};

extern UhdaController** Drv_uHDAControllers;
extern size_t Drv_uHDAControllerCount;

enum hda_ioctls {
    IOCTL_HDA_BASE_IOCTLs = 0x100,
    
    IOCTL_HDA_OUTPUT_STREAM_COUNT,
    IOCTL_HDA_OUTPUT_STREAM_SELECT,
    IOCTL_HDA_OUTPUT_STREAM_SELECTED,
    
    IOCTL_HDA_CODEC_COUNT,
    IOCTL_HDA_CODEC_SELECT,
    IOCTL_HDA_CODEC_SELECTED,
    
    IOCTL_HDA_CODEC_OUTPUT_GROUP_COUNT,
    IOCTL_HDA_CODEC_SELECT_OUTPUT_GROUP,
    IOCTL_HDA_CODEC_SELECTED_OUTPUT_GROUP,
    
    IOCTL_HDA_OUTPUT_GROUP_OUTPUT_COUNT,
    IOCTL_HDA_OUTPUT_GROUP_SELECT_OUTPUT,
    IOCTL_HDA_OUTPUT_GROUP_SELECTED_OUTPUT,
    
    IOCTL_HDA_OUTPUT_GET_PRESENCE,
    IOCTL_HDA_OUTPUT_GET_INFO,
    
    IOCTL_HDA_STREAM_SETUP,
    IOCTL_HDA_STREAM_PLAY,
    IOCTL_HDA_STREAM_QUEUE_DATA, // No parameters, but the next write will queue the data written
    IOCTL_HDA_STREAM_CLEAR_QUEUE,
    IOCTL_HDA_STREAM_SHUTDOWN,
    IOCTL_HDA_STREAM_GET_STATUS,
    IOCTL_HDA_STREAM_GET_REMAINING,
    IOCTL_HDA_STREAM_GET_BUFFER_SIZE,
    
    IOCTL_HDA_PATH_FIND,
    IOCTL_HDA_PATH_SETUP,
    IOCTL_HDA_PATH_SHUTDOWN,
    IOCTL_HDA_PATH_VOLUME,
    IOCTL_HDA_PATH_MUTE,

    // takes in a handle instead of a struct fd
    IOCTL_HDA_STREAM_SETUP_USER,
};

enum {
    FORMAT_PCM8,
    FORMAT_PCM16,
    FORMAT_PCM20,
    FORMAT_PCM24,
    FORMAT_PCM32,
};

typedef          size_t *hda_get_size_parameter;
typedef          size_t *hda_get_count_parameter;
typedef          size_t *hda_get_index_parameter;
typedef    const size_t *hda_set_index_parameter;
typedef            bool *hda_output_get_presence_parameter;
typedef const uintptr_t *hda_path_shutdown_parameter;
typedef struct stream_parameters {
    uint32_t sample_rate; // in hertz
    uint32_t channels;
    uint8_t format;
} stream_parameters;
typedef struct hda_stream_setup_parameters {
    stream_parameters stream_params;
    uint32_t ring_buffer_size;
    // doesn't need to be a pipe
    // can be nullptr
    fd* ring_buffer_pipe; 
} *hda_stream_setup_parameters;
typedef struct hda_stream_setup_user_parameters {
    stream_parameters stream_params;
    uint32_t ring_buffer_size;
    // doesn't need to be a pipe
    // can be nullptr
    handle ring_buffer_pipe; 
} *hda_stream_setup_user_parameters;
typedef const bool *hda_stream_play;
typedef struct hda_path_find_parameters {
    bool same_stream; // whether all paths will be playing the same stream.
    size_t other_path_count;
    uintptr_t found_path; // output.
    uintptr_t other_paths[];
} *hda_path_find_parameters;
typedef struct hda_path_setup_parameters {
    uintptr_t path;
    stream_parameters stream_parameters; // stream parameters hint<-->actual stream parameters
} *hda_path_setup_parameters;
typedef UhdaStreamStatus* hda_path_get_status_parameter;
struct hda_path_boolean_parameter {
    uintptr_t path;
    bool par1;
};
struct hda_path_byte_parameter {
    uintptr_t path;
    uint8_t par1;
};
typedef const struct hda_path_boolean_parameter *hda_path_mute_parameter;
typedef const struct hda_path_byte_parameter *hda_path_volume_parameter;

typedef struct audio_dev
{
    UhdaController* controller;

    const UhdaCodec* const* codecs;
    size_t codec_count;
    const UhdaCodec* selected_codec;
    size_t selected_codec_idx;

    const UhdaOutputGroup* const* output_groups;
    size_t output_group_count;
    const UhdaOutputGroup* selected_output_group;
    size_t selected_output_group_idx;
    
    const UhdaOutput* const* outputs;
    size_t output_count;
    const UhdaOutput* selected_output;
    size_t selected_output_idx;
    
    UhdaStream** output_streams;
    size_t output_stream_count;
    UhdaStream* selected_output_stream;
    size_t selected_output_stream_idx;

    bool next_write_is_data_queue;

    vnode* vn;
    dirent* dent;
    char* name;
} audio_dev;

void OBOS_InitializeHDAAudioDev()
{
    const char* name_format = "hdaaudio%lu";
    for (size_t i = 0; i < Drv_uHDAControllerCount; i++)
    {
        audio_dev* dev = ZeroAllocate(Vfs_Allocator, 1, sizeof(audio_dev), nullptr);
        dev->controller = Drv_uHDAControllers[i];
        uhda_get_codecs(dev->controller, &dev->codecs, &dev->codec_count);
        uhda_get_output_streams(dev->controller, &dev->output_streams, &dev->output_stream_count);
        dev->selected_codec_idx = SIZE_MAX;
        dev->selected_output_stream_idx = SIZE_MAX;
        dev->selected_output_idx = SIZE_MAX;
        dev->selected_output_group_idx = SIZE_MAX;
        dev->selected_codec = nullptr;
        dev->selected_output_stream = nullptr;
        dev->selected_output = nullptr;
        dev->selected_output_group = nullptr;
        dev->output_count = 0;
        dev->output_group_count = 0;
        dev->next_write_is_data_queue = false;

        size_t len_name = snprintf(nullptr, 0, name_format, i);
        dev->name = Allocate(OBOS_KernelAllocator, len_name+1, nullptr);
        snprintf(dev->name, len_name+1, name_format, i);

        dev->vn = Drv_AllocateVNode(&HDADriver, (dev_desc)dev, 0, nullptr, VNODE_TYPE_CHR);
        dev->dent = Drv_RegisterVNode(dev->vn, dev->name);
    }
}

obos_status hda_write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    audio_dev* dev = (void*)desc;
    if (!dev->next_write_is_data_queue)
        return OBOS_STATUS_SUCCESS;
    if (!dev->selected_output_stream)
        return OBOS_STATUS_UNINITIALIZED;
    uint32_t count = blkCount;
    uhda_stream_queue_data(dev->selected_output_stream, buf, &count);
    if (nBlkWritten)
        *nBlkWritten = count;
    return OBOS_STATUS_SUCCESS;
}

struct fd_fill_userdata
{
    fd* pipe;
    dpc dpc;
    thread* thr;
    event wake_thr;
    bool request_thread_kill;
    void* dest_buffer;
    uint32_t space;
};

void fd_fill_worker(void* user)
{
    struct fd_fill_userdata *udata = user;
    while (!udata->request_thread_kill)
    {
        Core_WaitOnObject(WAITABLE_OBJECT(udata->wake_thr));
        Core_EventClear(&udata->wake_thr);
        if (udata->request_thread_kill)
            break;
        Vfs_FdRead(udata->pipe, udata->dest_buffer, udata->space, nullptr);
    }
    Core_ExitCurrentThread();
    
}
void fd_fill_fn_start_thread(dpc* d, void* arg)
{
    OBOS_UNUSED(d);
    struct fd_fill_userdata *udata = arg;
    if (udata->thr)
    {
        Core_EventSet(&udata->wake_thr, false);
        return;
    }
    udata->wake_thr = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    udata->thr = CoreH_ThreadAllocate(nullptr);
    thread_ctx ctx = {};
    CoreS_SetupThreadContext(&ctx, (uintptr_t)fd_fill_worker, (uintptr_t)arg, false, 
        Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x4000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr), 0x4000);
    CoreH_ThreadInitialize(udata->thr, THREAD_PRIORITY_URGENT, Core_DefaultThreadAffinity, &ctx);
    CoreH_ThreadReady(udata->thr);
    Core_ProcessAppendThread(OBOS_KernelProcess, udata->thr);
    // Vfs_FdRead(udata->pipe, udata->dest_buffer, udata->space, nullptr);
}

uint32_t fd_fill_fn(void* arg, void* buffer, uint32_t space)
{
    struct fd_fill_userdata *udata = arg;
    udata->dest_buffer = buffer;
    udata->space = space;
    udata->dpc.userdata = udata;
    // fd_fill_fn_impl(0, udata);
    CoreH_InitializeDPC(&udata->dpc, fd_fill_fn_start_thread, CoreH_CPUIdToAffinity(CoreS_GetCPULocalPtr()->id));
    return space;
}

obos_status ioctl(dev_desc what, uint32_t request, void* argpv)
{
    audio_dev* dev = (void*)what;
    union {
        void* ptr;
        hda_get_count_parameter get_count;
        hda_get_index_parameter get_index;
        hda_get_size_parameter get_size;
        hda_set_index_parameter set_index;
        hda_output_get_presence_parameter output_get_presence;
        UhdaOutputInfo* output_get_info;
        UhdaStreamStatus* stream_get_status;
        hda_path_shutdown_parameter path_shutdown;
        hda_path_setup_parameters path_setup;
        hda_path_find_parameters path_find;
        hda_path_get_status_parameter path_get_status;
        hda_stream_setup_parameters stream_setup;
        hda_stream_setup_user_parameters stream_setup_user;
        hda_stream_play stream_play;
        hda_path_volume_parameter path_volume;
        hda_path_mute_parameter path_mute;
    } argp = {.ptr=argpv};
    switch (request) {
        case IOCTL_HDA_CODEC_COUNT:
            *argp.get_count = dev->codec_count;
            break;
        case IOCTL_HDA_OUTPUT_GROUP_OUTPUT_COUNT:
            *argp.get_count = dev->output_count;
            break;
        case IOCTL_HDA_CODEC_OUTPUT_GROUP_COUNT:
            *argp.get_count = dev->output_group_count;
            break;
        case IOCTL_HDA_OUTPUT_STREAM_COUNT:
            *argp.get_count = dev->output_stream_count;
            break;

        case IOCTL_HDA_OUTPUT_STREAM_SELECT:
            if (dev->output_stream_count <= *argp.set_index)
                return OBOS_STATUS_INVALID_ARGUMENT;
            dev->selected_output_stream = dev->output_streams[*argp.set_index];
            dev->selected_output_stream_idx = *argp.set_index;
            break;
        case IOCTL_HDA_CODEC_SELECT_OUTPUT_GROUP:
            if (dev->output_group_count <= *argp.set_index)
                return OBOS_STATUS_INVALID_ARGUMENT;
            dev->selected_output_group = dev->output_groups[*argp.set_index];
            dev->selected_output_group_idx = *argp.set_index;
            dev->selected_output = 0;
            dev->selected_output_idx = SIZE_MAX;
            uhda_output_group_get_outputs(dev->selected_output_group, &dev->outputs, &dev->output_count);
            break;
        case IOCTL_HDA_OUTPUT_GROUP_SELECT_OUTPUT:
            if (dev->output_count <= *argp.set_index)
                return OBOS_STATUS_INVALID_ARGUMENT;
            dev->selected_output = dev->outputs[*argp.set_index];
            dev->selected_output_idx = *argp.set_index;
            break;
        case IOCTL_HDA_CODEC_SELECT:
            if (dev->codec_count <= *argp.set_index)
                return OBOS_STATUS_INVALID_ARGUMENT;
            dev->selected_codec = dev->codecs[*argp.set_index];
            dev->selected_codec_idx = *argp.set_index;
            dev->selected_output_group = 0;
            dev->selected_output_group_idx = SIZE_MAX;
            uhda_codec_get_output_groups(dev->selected_codec, &dev->output_groups, &dev->output_group_count);
            break;

        case IOCTL_HDA_OUTPUT_GROUP_SELECTED_OUTPUT:
            *argp.get_index = dev->selected_output_idx;
            break;
        case IOCTL_HDA_OUTPUT_STREAM_SELECTED:
            *argp.get_index = dev->selected_output_stream_idx;
            break;
        case IOCTL_HDA_CODEC_SELECTED:
            *argp.get_index = dev->selected_codec_idx;
            break;
        case IOCTL_HDA_CODEC_SELECTED_OUTPUT_GROUP:
            *argp.get_index = dev->selected_output_group_idx;
            break;
        
        case IOCTL_HDA_OUTPUT_GET_PRESENCE:
            if (dev->selected_output)
                uhda_output_get_presence(dev->selected_output, argp.output_get_presence);
            else return OBOS_STATUS_UNINITIALIZED;
            break;
        case IOCTL_HDA_OUTPUT_GET_INFO:
            if (dev->selected_output)
                *argp.output_get_info = uhda_output_get_info(dev->selected_output);
            else return OBOS_STATUS_UNINITIALIZED;
            break;
        case IOCTL_HDA_STREAM_PLAY:
            if (dev->selected_output_stream)
                uhda_stream_play(dev->selected_output_stream, *argp.stream_play);
            else return OBOS_STATUS_UNINITIALIZED;
            break; 
        case IOCTL_HDA_STREAM_GET_BUFFER_SIZE:
            if (dev->selected_output_stream)
                *argp.get_size = uhda_stream_get_buffer_size(dev->selected_output_stream);
            else return OBOS_STATUS_UNINITIALIZED;
            break;
        case IOCTL_HDA_STREAM_GET_STATUS:
            if (dev->selected_output_stream)
                *argp.stream_get_status = uhda_stream_get_status(dev->selected_output_stream);
            else return OBOS_STATUS_UNINITIALIZED;
            break;
        case IOCTL_HDA_STREAM_CLEAR_QUEUE:
            if (dev->selected_output_stream)
                uhda_stream_clear_queue(dev->selected_output_stream);
            else return OBOS_STATUS_UNINITIALIZED;
            break;
        case IOCTL_HDA_STREAM_SHUTDOWN:
            if (dev->selected_output_stream)
                uhda_stream_shutdown(dev->selected_output_stream);
            else return OBOS_STATUS_UNINITIALIZED;
            break;
        case IOCTL_HDA_STREAM_QUEUE_DATA:
            if (dev->selected_output_stream)
                dev->next_write_is_data_queue = true;
            else return OBOS_STATUS_UNINITIALIZED;
            break;
        case IOCTL_HDA_STREAM_SETUP:
        {
            if (!dev->selected_output_stream)
                return OBOS_STATUS_UNINITIALIZED;
            
            UhdaStreamParams params = {};
            params.channels = argp.stream_setup->stream_params.channels;
            params.sample_rate = argp.stream_setup->stream_params.sample_rate;
            params.fmt = argp.stream_setup->stream_params.format;
            if (params.fmt > UHDA_FORMAT_PCM32 || params.fmt < 0)
                return OBOS_STATUS_INVALID_ARGUMENT;
            struct fd_fill_userdata *udata = nullptr;
            if (argp.stream_setup->ring_buffer_pipe)
            {
                udata = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(struct fd_fill_userdata), nullptr);
                udata->pipe = argp.stream_setup->ring_buffer_pipe;
            }
            uhda_stream_setup(dev->selected_output_stream, 
                              &params, 
                              argp.stream_setup->ring_buffer_size, 
                              argp.stream_setup->ring_buffer_pipe ? fd_fill_fn : nullptr, udata, 
                              0, nullptr, nullptr);
            break;
        }
        case IOCTL_HDA_STREAM_SETUP_USER:
        {
            struct hda_stream_setup_parameters real_params = {};
            real_params.stream_params = argp.stream_setup_user->stream_params;
            real_params.ring_buffer_size = argp.stream_setup_user->ring_buffer_size;
            if (HANDLE_TYPE(argp.stream_setup_user->ring_buffer_pipe) != HANDLE_TYPE_INVALID)
            {
                obos_status status = OBOS_STATUS_SUCCESS;
                handle_desc* pipe_desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), argp.stream_setup_user->ring_buffer_pipe, HANDLE_TYPE_FD, false, &status);
                if (obos_is_error(status))
                    return status;
                fd *pipe = pipe_desc->un.fd;
                real_params.ring_buffer_pipe = pipe;
            }
            ioctl(what, IOCTL_HDA_STREAM_SETUP, &real_params);
            break;
        }

        // TODO: Make path ioctls safer?
        case IOCTL_HDA_PATH_MUTE:
            uhda_path_mute((UhdaPath *)argp.path_mute->path, argp.path_mute->par1);
            break;
        case IOCTL_HDA_PATH_VOLUME: 
            uhda_path_set_volume((UhdaPath *)argp.path_volume->path, argp.path_volume->par1);
            break;
        case IOCTL_HDA_PATH_SHUTDOWN: 
            uhda_path_shutdown((UhdaPath *)argp.path_volume->path);
            break;
        case IOCTL_HDA_PATH_SETUP:
        {
            UhdaStreamParams params = {};
            params.channels = argp.path_setup->stream_parameters.channels;
            params.sample_rate = argp.path_setup->stream_parameters.sample_rate;
            params.fmt = argp.path_setup->stream_parameters.format;
            if (params.fmt > UHDA_FORMAT_PCM32 || params.fmt < 0)
                return OBOS_STATUS_INVALID_ARGUMENT;
            uhda_path_setup((UhdaPath *)argp.path_setup->path, &params, dev->selected_output_stream);
            argp.path_setup->stream_parameters.format = params.fmt;
            argp.path_setup->stream_parameters.sample_rate = params.sample_rate;
            argp.path_setup->stream_parameters.channels = params.channels;
            break;
        }
        case IOCTL_HDA_PATH_FIND:
        {
            if (((intptr_t)argp.path_find->other_path_count) < 0)
                return OBOS_STATUS_INVALID_ARGUMENT;
            const UhdaPath* other_paths[argp.path_find->other_path_count+1];
            memcpy(other_paths, argp.path_find->other_paths, argp.path_find->other_path_count*sizeof(uintptr_t));
            uhda_find_path(dev->selected_output,
                           other_paths, 
                           argp.path_find->other_path_count, 
                           argp.path_find->same_stream, 
                           (UhdaPath**)&argp.path_find->found_path);
            break;
        }
        default:
            return OBOS_STATUS_INVALID_IOCTL;
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status ioctl_argp_size(uint32_t request, size_t* osize)
{
    enum hda_ioctls req = request;
    switch (req) {
        case IOCTL_HDA_CODEC_COUNT:
        case IOCTL_HDA_OUTPUT_GROUP_OUTPUT_COUNT:
        case IOCTL_HDA_CODEC_OUTPUT_GROUP_COUNT:
        case IOCTL_HDA_OUTPUT_STREAM_COUNT:
            *osize = sizeof(*(hda_get_count_parameter)nullptr);
            break; 
        case IOCTL_HDA_OUTPUT_STREAM_SELECT:
        case IOCTL_HDA_OUTPUT_GROUP_SELECT_OUTPUT:
        case IOCTL_HDA_CODEC_SELECT_OUTPUT_GROUP:
        case IOCTL_HDA_CODEC_SELECT:
            *osize = sizeof(*(hda_set_index_parameter)nullptr);
            break; 
        case IOCTL_HDA_OUTPUT_GROUP_SELECTED_OUTPUT:
        case IOCTL_HDA_OUTPUT_STREAM_SELECTED:
        case IOCTL_HDA_CODEC_SELECTED:
        case IOCTL_HDA_CODEC_SELECTED_OUTPUT_GROUP:
            *osize = sizeof(*(hda_get_index_parameter)nullptr);
            break; 
        case IOCTL_HDA_OUTPUT_GET_PRESENCE:
            *osize = sizeof(*(hda_output_get_presence_parameter)nullptr);
            break;
        case IOCTL_HDA_OUTPUT_GET_INFO: 
            *osize = sizeof(UhdaOutputInfo);
            break;
        case IOCTL_HDA_STREAM_PLAY:
            *osize = sizeof(*(hda_stream_play)nullptr);
            break; 
        case IOCTL_HDA_STREAM_GET_BUFFER_SIZE:
            *osize = sizeof(*(hda_get_size_parameter)nullptr);
            break;
        case IOCTL_HDA_STREAM_GET_STATUS:
            *osize = sizeof(*(hda_path_get_status_parameter)nullptr);
            break;
        // Both of these take no parameters*
        // *queue data simply queues the data written on the next WRITE IRP or write_sync
        case IOCTL_HDA_STREAM_CLEAR_QUEUE:
        case IOCTL_HDA_STREAM_QUEUE_DATA:
        case IOCTL_HDA_STREAM_SHUTDOWN:
            *osize = 0;
            break;
        case IOCTL_HDA_STREAM_SETUP:
            *osize = sizeof(*(hda_stream_setup_parameters)nullptr);
            break;
        case IOCTL_HDA_STREAM_SETUP_USER:
            *osize = sizeof(*(hda_stream_setup_user_parameters)nullptr);
            break;
        case IOCTL_HDA_PATH_MUTE: 
            *osize = sizeof(*(hda_path_mute_parameter)nullptr);
            break;
        case IOCTL_HDA_PATH_VOLUME: 
            *osize = sizeof(*(hda_path_volume_parameter)nullptr);
            break;
        case IOCTL_HDA_PATH_SHUTDOWN: 
            *osize = sizeof(*(hda_path_shutdown_parameter)nullptr);
            break;
        case IOCTL_HDA_PATH_SETUP:
            *osize = sizeof(*(hda_path_setup_parameters)nullptr);
            break;
        case IOCTL_HDA_PATH_FIND:
            *osize = sizeof(*(hda_path_find_parameters)nullptr);
            break;
        default:
            return OBOS_STATUS_INVALID_IOCTL;
    }
    return OBOS_STATUS_SUCCESS;
}
