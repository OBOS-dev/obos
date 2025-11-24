/*
 * drivers/test_driver/fireworks.c
 *
 * Copyright (c) 2024 Omar Berrow
 *
 * Inspired from BORON's fireworks test.
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>
#include <text.h>

#include <scheduler/thread_context_info.h>
#include <scheduler/thread.h>
#include <scheduler/process.h>

#include <mm/context.h>
#include <mm/alloc.h>

#include <irq/timer.h>

#include <locks/wait.h>
#include <locks/event.h>
#include <locks/spinlock.h>

#include <allocators/base.h>

#include <external/fixedptc.h>

#include "rand.h"

#undef OBOS_TEXT_BACKGROUND

#ifndef OBOS_TEXT_BACKGROUND
#   define OBOS_TEXT_BACKGROUND 0x00000000
#endif

void delay_impl(void* userdata)
{
	Core_EventSet((event*)userdata, true);
}
static timer* delay(timer_tick ms, timer* cached_t)
{
	// event e = EVENT_INITIALIZE(EVENT_NOTIFICATION);
	// timer* t = cached_t ? cached_t : Core_TimerObjectAllocate(nullptr);
	// memzero(t, sizeof(*t));
	// t->handler = delay_impl;
	// t->userdata = &e;
	// // printf("wait for %d ms\n", ms);
	// obos_status status = Core_TimerObjectInitialize(t, TIMER_MODE_DEADLINE, ms*1000);
	// OBOS_ASSERT(status == OBOS_STATUS_SUCCESS && "Core_TimerObjectInitialize");
	// status = Core_WaitOnObject(WAITABLE_OBJECT(e));
	// OBOS_ASSERT(status == OBOS_STATUS_SUCCESS && "Core_WaitOnObject");
	// return t;
	OBOS_UNUSED(cached_t);
	// irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
	timer_tick deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(ms*1000);
	while(CoreS_GetTimerTick() < deadline)
		OBOSS_SpinlockHint();
	// Core_LowerIrql(oldIrql);
	return nullptr;
}
#define FRAMEBUFFER_WIDTH (OBOS_TextRendererState.fb.width)
#define FRAMEBUFFER_HEIGHT (OBOS_TextRendererState.fb.height)
typedef struct thr_free_stack
{
	void* base; // 0x10000 bytes
	struct thr_free_stack *next, *prev;
} thr_free_stack;
typedef struct thr_free_stack_list
{
	thr_free_stack *head, *tail;
	size_t nNodes;
	spinlock lock;
} thr_free_stack_list;
thr_free_stack_list free_thread_stacks = {};
void reuse_stack(void* base, size_t sz, void* userdata)
{
	OBOS_UNUSED(sz);
	OBOS_UNUSED(userdata);
	thr_free_stack *node = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(*node), nullptr);
	node->base = base;
	irql oldIrql = Core_SpinlockAcquire(&free_thread_stacks.lock);
	if (!free_thread_stacks.head)
		free_thread_stacks.head = node;
	if (free_thread_stacks.tail)
		free_thread_stacks.tail->next = node;
	node->prev = free_thread_stacks.tail;
	free_thread_stacks.tail = node;
	free_thread_stacks.nNodes++;
	Core_SpinlockRelease(&free_thread_stacks.lock, oldIrql);
}
thread* create_thread(void* entry, uintptr_t udata, thread_priority priority, thread** out)
{
    thread* new = CoreH_ThreadAllocate(nullptr);
    thread_ctx ctx = {};
    void* stack = nullptr;
	irql oldIrql = Core_SpinlockAcquire(&free_thread_stacks.lock);
	if (free_thread_stacks.nNodes)
	{
		thr_free_stack* node = free_thread_stacks.head;
		stack = node->base;
		if (node == free_thread_stacks.head)
			free_thread_stacks.head = node->next;
		if (node == free_thread_stacks.tail)
			free_thread_stacks.tail = node->prev;
		if (node->next)
			node->next->prev = node->prev;
		if (node->prev)
			node->prev->next = node->next;
		free_thread_stacks.nNodes--;
		Core_SpinlockRelease(&free_thread_stacks.lock, oldIrql);
		Free(OBOS_NonPagedPoolAllocator, node, sizeof(*node));
	}
	else 
	{
		Core_SpinlockRelease(&free_thread_stacks.lock, oldIrql);
		stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext,
			nullptr, 0x10000,
			0, VMA_FLAGS_KERNEL_STACK,
			nullptr, nullptr);
	}
    CoreS_SetupThreadContext(&ctx, (uintptr_t)entry, udata, false, stack, 0x10000);
    CoreH_ThreadInitialize(new, priority, 0b1, &ctx);
    new->references++;
    // new->stackFree = CoreH_VMAStackFree;
    // new->stackFreeUserdata = &Mm_KernelContext;
    new->stackFree = reuse_stack;
	if (out)
	{
		new->references++;
		*out = new;
	}
    Core_ProcessAppendThread(OBOS_KernelProcess, new);
	CoreH_ThreadReady(new);
    return new;
}
void plot_pixel(uint32_t rgbx, int32_t x, int32_t y)
{
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	OBOS_ASSERT (x < (int32_t)FRAMEBUFFER_WIDTH);
	OBOS_ASSERT(y < (int32_t)FRAMEBUFFER_HEIGHT);
	if (x > (int32_t)FRAMEBUFFER_WIDTH || y > (int32_t)FRAMEBUFFER_HEIGHT)
		return;
	// uint8_t* fb = OBOS_TextRendererState.fb.backbuffer_base ? OBOS_TextRendererState.fb.backbuffer_base : OBOS_TextRendererState.fb.base;
    uint8_t* fb = OBOS_TextRendererState.fb.base;
    fb += (y*OBOS_TextRendererState.fb.pitch+x*OBOS_TextRendererState.fb.bpp/8);
    OBOS_PlotPixel(rgbx, fb, OBOS_TextRendererState.fb.format);
}
uint32_t random_pixel()
{
    return ((mt_random() + 0x808080) & 0xffffff) << 8;
}

typedef struct firework_data
{
	int32_t x,y;
	uint32_t rgbx;
	fixedptd act_x, act_y;
	fixedptd vel_x, vel_y;
	int explosion_range;
	_Atomic(size_t) refcount;
	bool can_free;
	bool stress_test;
	fixedptd direction;
} firework_data;

OBOS_NO_UBSAN fixedptd fp_rand_sign()
{
	if (mt_random() % 2)
		return -fixedpt_fromint(mt_random() % (FIXEDPT_FBITS+1));
	return fixedpt_fromint(mt_random() % (FIXEDPT_FBITS+1));
}

#include "sin_table.h"
OBOS_NO_UBSAN fixedptd sin(int angle)
{
	return fixedpt_xdiv(fixedpt_fromint(sin_table[angle % 65536]), fixedpt_fromint(32768));
}
OBOS_NO_UBSAN fixedptd cos(int angle)
{
	return sin(angle+16384);
}

static void particle_update(void* udata_)
{
	uint64_t *udata = udata_;
	firework_data *data = (void*)(uintptr_t)udata[0];
	event* e = (void*)(uintptr_t)udata[1];
	timer* t = (void*)(uintptr_t)udata[2];
	int expires_in = (int)(uintptr_t)udata[3];
	timer_tick startedAt = (timer_tick)(uintptr_t)udata[4];
	if ((expires_in + startedAt) >= t->lastTimeTicked)
	{
		Core_CancelTimer(t);
		Core_EventSet(e, true);
		return;
	}
	if (data->y < 0)
	{
		Core_CancelTimer(t);
		Core_EventSet(e, true);
		return;
	}
	if (data->x < 0)
	{
		Core_CancelTimer(t);
		Core_EventSet(e, true);
		return;
	}
	if (data->y >= (int32_t)FRAMEBUFFER_HEIGHT)
	{
		Core_CancelTimer(t);
		Core_EventSet(e, true);
		return;
	}
	if (data->x >= (int32_t)FRAMEBUFFER_WIDTH)
	{
		Core_CancelTimer(t);
		Core_EventSet(e, true);
		return;
	}
	plot_pixel(OBOS_TEXT_BACKGROUND, data->x, data->y);
	fixedptd temp_pt = fixedpt_div(fixedpt_fromint(17), fixedpt_fromint(1000));
	data->act_x = fixedpt_add(fixedpt_mul(data->vel_x, temp_pt), data->act_x);
	data->act_y = fixedpt_add(data->act_y, fixedpt_mul(data->vel_y, temp_pt));
	data->x = fixedpt_toint(data->act_x);
	data->y = fixedpt_toint(data->act_y);
	if (data->y < 0)
	{
		Core_CancelTimer(t);
		Core_EventSet(e, true);
		return;
	}
	if (data->x < 0)
	{
		Core_CancelTimer(t);
		Core_EventSet(e, true);
		return;
	}
	if (data->y >= (int32_t)FRAMEBUFFER_HEIGHT)
	{
		Core_CancelTimer(t);
		Core_EventSet(e, true);
		return;
	}
	if (data->x >= (int32_t)FRAMEBUFFER_WIDTH)
	{
		Core_CancelTimer(t);
		Core_EventSet(e, true);
		return;
	}
	data->vel_y += fixedpt_fromint(10)*temp_pt;
	plot_pixel(data->rgbx, data->x, data->y);
}
_Atomic(size_t) nParticlesLeft = 0;
void particle_handler(void* udata)
{
	firework_data* parent = udata;
	firework_data data = {.x=parent->x,.y=parent->y,.act_x=parent->act_x,.act_y=parent->act_y,.direction=parent->direction,.stress_test = parent->stress_test};
	int ExplosionRange = parent->explosion_range;
	if (!(--parent->refcount) && parent->can_free)
	{
		Free(OBOS_NonPagedPoolAllocator, parent, sizeof(*parent));
		parent = nullptr;
	}
	int Angle = mt_random() % 65536;
	data.vel_x = fixedpt_mul(cos(Angle), fp_rand_sign())*ExplosionRange;
	data.vel_y = fixedpt_mul(sin(Angle), fp_rand_sign())*ExplosionRange;
	const int expires_in = 2000 + mt_random() % 2000;
	data.rgbx = random_pixel();
	if (!data.stress_test)
	{
		timer t = {};
		event e = {};
		uint64_t udata[] = {
			(uintptr_t)&data,
			(uintptr_t)&e,
			(uintptr_t)&t,
			expires_in, 0
		};
		t.handler = particle_update;
		t.userdata = udata;
		obos_status status = Core_TimerObjectInitialize(&t, TIMER_MODE_INTERVAL, 17*1000 /* 17 ms */);
		udata[3] = t.lastTimeTicked;
		if (obos_is_error(status))
			goto die;
		status = Core_WaitOnObject(WAITABLE_OBJECT(e));
		goto die;
	}
	int t = 0;
	timer* timer = nullptr;
	for (int i = 0; i < expires_in; )
	{
		plot_pixel(data.rgbx, data.x, data.y);
		int curr_delay = 8+(t!=0);
		timer = delay(curr_delay, timer);
		i += curr_delay;
		if (++t == 3)
			t = 0;
		plot_pixel(OBOS_TEXT_BACKGROUND, data.x, data.y);
		fixedptd temp_pt = fixedpt_div(fixedpt_fromint(curr_delay), fixedpt_fromint(1000));
		data.act_x = fixedpt_add(fixedpt_mul(data.vel_x, temp_pt), data.act_x);
		data.act_y = fixedpt_add(data.act_y, fixedpt_mul(data.vel_y, temp_pt));
		data.x = fixedpt_toint(data.act_x);
		data.y = fixedpt_toint(data.act_y);
		if (data.y < 0)
			break;
		if (data.x < 0)
			break;
		if (data.y >= (int32_t)FRAMEBUFFER_HEIGHT)
			break;
		if (data.x >= (int32_t)FRAMEBUFFER_WIDTH)
			break;
		data.vel_y += (-fixedpt_fromint(10))*temp_pt;
	}
	die:
	nParticlesLeft--;
	Core_ExitCurrentThread();
}
static void explodeable_handler(bool stress_test)
{
	firework_data data = {};
	int x_offset = (FRAMEBUFFER_WIDTH*400)/1024;
	data.x = FRAMEBUFFER_WIDTH / 2;
	data.y = FRAMEBUFFER_HEIGHT - 1;
	data.stress_test = stress_test;
	data.act_x = fixedpt_fromint(data.x);
	data.act_y = fixedpt_fromint(data.y);
	data.vel_y = -fixedpt_fromint((400+mt_random()%400));
	data.direction = mt_random() % 2 ? -fixedpt_fromint(1) : fixedpt_fromint(1);
	data.vel_x = fixedpt_mul(fixedpt_fromint(x_offset), data.direction);
	data.rgbx = random_pixel();
	data.explosion_range = mt_random() % 100 + 100;
	int expires_in = 500 + mt_random() % 500;
	int t = 0;
	timer* timer = nullptr;
	for (int i = 0; i < expires_in; )
	{
		plot_pixel(data.rgbx, data.x, data.y);
		int curr_delay = 16+(t!=0);
		timer = delay(curr_delay, timer);
		i += curr_delay;
		if (++t == 3)
			t = 0;
		plot_pixel(OBOS_TEXT_BACKGROUND, data.x, data.y);
		fixedptd temp_pt = fixedpt_div(fixedpt_fromint(curr_delay), fixedpt_fromint(1000));
		data.act_x = fixedpt_add(fixedpt_mul(data.vel_x, temp_pt), data.act_x);
		data.act_y = fixedpt_add(data.act_y, fixedpt_mul(data.vel_y, temp_pt));
		data.x = fixedpt_toint(data.act_x);
		data.y = fixedpt_toint(data.act_y);
		if (data.y < 0)
			break;
		if (data.x < 0)
			break;
		if (data.y >= (int32_t)FRAMEBUFFER_HEIGHT)
			break;
		if (data.x >= (int32_t)(FRAMEBUFFER_WIDTH))
			break;
		data.vel_y += -fixedpt_fromint(10)*temp_pt;
	}
	int nParticles = mt_random() % 100 + 100;
	firework_data* fw_clone = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(firework_data), nullptr);
	memcpy(fw_clone, &data, sizeof(data));
	nParticlesLeft += nParticles;
	for (int i = 0; i < nParticles; i++)
	{
		fw_clone->can_free = false;
		fw_clone->refcount++;
		create_thread((void*)particle_handler, (uintptr_t)fw_clone, THREAD_PRIORITY_HIGH, nullptr);
	}
	fw_clone->can_free = true;
	Core_ExitCurrentThread();
}
static void SpawnNewExplodable(bool stress_test, thread** out)
{
	create_thread((void*)explodeable_handler, stress_test, THREAD_PRIORITY_NORMAL, out);
}

DRV_EXPORT void TestDriver_Fireworks(uint32_t max_iterations, int spawn_min, int spawn_max, bool stress_test)
{
	OBOS_UNUSED(max_iterations);
	memset(OBOS_TextRendererState.fb.base, 0, OBOS_TextRendererState.fb.pitch*OBOS_TextRendererState.fb.height);
    mt_seed(random_seed());
    timer* t = nullptr;
	log_level old = OBOS_GetLogLevel();
	OBOS_SetLogLevel(LOG_LEVEL_ERROR);
	for (uint32_t i = 0; i < max_iterations; i++)
	{
		int nToSpawn = mt_random() % spawn_max + spawn_min;
		for (int i = 0; i < nToSpawn; i++)
			SpawnNewExplodable(stress_test, nullptr);
		// Wait 1-2 seconds to spawn new fireworks.
		t = delay(mt_random() % 2000 + 2000, t);
		// while (threadCounts[last_direction])
		// 	OBOSS_SpinlockHint();
	}
	OBOS_SetLogLevel(old);
	Core_TimerObjectFree(t);
}
