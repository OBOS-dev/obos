#if OBOS_ENABLE_PROFILING

#include "prof.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <scheduler/schedule.h>

#define current_task (Core_GetCurrentThread())

typedef struct {
    void *fn;
    uint64_t total;
    size_t calls;
} record_t;

#define MAX_RECORDS 0x10000 // should be greater than the total number of functions across the kernel, 64K is completely arbitrary
static record_t records[MAX_RECORDS];
static size_t num_records = 0;

static int prof_lock;
static int prof_active;

__attribute__((no_instrument_function)) static unsigned long sdi(void) {
    unsigned long value;
    asm volatile("pushfq; popq %0; cli" : "=rm"(value));
    while (__atomic_exchange_n(&prof_lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
    return value;
}

__attribute__((no_instrument_function)) static void ri(unsigned long value) {
    __atomic_store_n(&prof_lock, 0, __ATOMIC_RELEASE);
        if (value & 0x200) asm("sti");
}

__attribute__((no_instrument_function)) void prof_start(void) {
    __atomic_store_n(&prof_active, 1, __ATOMIC_RELEASE);
}

__attribute__((no_instrument_function)) void prof_stop(void) {
    __atomic_store_n(&prof_active, 0, __ATOMIC_RELEASE);
}

__attribute__((no_instrument_function)) void prof_reset(void) {
    unsigned long state = sdi();
    num_records = 0;
    ri(state);
}

__attribute__((no_instrument_function)) static void printc(char c) {
    asm("out %0, $0xe9" ::"a"(c));
}

__attribute__((no_instrument_function)) static void prints(const char *s) {
    for (char c = *s; c != 0; c = *++s) printc(c);
}

__attribute__((no_instrument_function)) static void printu(uint64_t value) {
    char buffer[sizeof(value) * 3 + 1];
    size_t index = sizeof(buffer);
    buffer[--index] = 0;

    do {
        buffer[--index] = '0' + (value % 10);
        value /= 10;
    } while (value > 0);

    prints(&buffer[index]);
}

__attribute__((no_instrument_function)) static void printx(uintptr_t value) {
    char buffer[sizeof(value) * 2 + 1];
    size_t index = sizeof(buffer);
    buffer[--index] = 0;

    do {
        buffer[--index] = "0123456789abcdef"[value & 15];
        value >>= 4;
    } while (value > 0);

    prints(&buffer[index]);
}

__attribute__((no_instrument_function)) static bool pred(record_t *a, record_t *b) {
    size_t ta = (a->total + (a->calls / 2)) / a->calls;
    size_t tb = (b->total + (b->calls / 2)) / b->calls;
    return ta < tb;
}

__attribute__((no_instrument_function)) void prof_show(const char *name) {
    unsigned long state = sdi();

    // sort it
    for (size_t i = 1; i < num_records; i++) {
        for (size_t j = i; j > 0 && pred(&records[j - 1], &records[j]); j--) {
            record_t *a = &records[j - 1];
            record_t *b = &records[j];

            record_t temp = *a;
            *a = *b;
            *b = temp;
        }
    }

    // print it
    prints("profiler results for '");
    prints(name);
    prints("' (");
    printu(num_records);
    prints(" records):\n");

    for (size_t i = 0; i < num_records; i++) {
        printu(i + 1);
        prints(". 0x");
        printx((uintptr_t)records[i].fn);
        prints(": ");
        printu(records[i].total);
        prints(" (");
        printu(records[i].calls);
        prints(" calls, avg ");
        printu((records[i].total + (records[i].calls / 2)) / records[i].calls);
        prints(" per call)\n");
    }

    ri(state);
}

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *call_site) {
    uint64_t start = __builtin_ia32_rdtsc();
    if (!__atomic_load_n(&prof_active, __ATOMIC_ACQUIRE)) return;
    if (!current_task) return;

    size_t idx = current_task->cur_frame++;
    if (idx >= MAX_FRAMES) {
        asm("cli");
        prints("\ntoo many frames\n");
        for (;;) asm("hlt");
    }

    call_frame_t *frame = &current_task->frames[idx];
    frame->fn = fn;
    frame->site = call_site;
    frame->ptime = 0;

    uint64_t end = __builtin_ia32_rdtsc();
    uint64_t time = end - start;

    for (size_t i = 0; i < idx; i++) {
        current_task->frames[i].ptime += time;
    }

    frame->start = __builtin_ia32_rdtsc();
}

__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *call_site) {
    uint64_t start = __builtin_ia32_rdtsc();
    if (!__atomic_load_n(&prof_active, __ATOMIC_ACQUIRE)) return;
    if (!current_task) return;

    unsigned state = sdi();

    size_t idx = --current_task->cur_frame;
    call_frame_t *frame = &current_task->frames[idx];
    uint64_t time = start - frame->start - frame->ptime;

    if (frame->fn != fn && frame->site != call_site) {
        asm("cli");
        prints("\nframe mismatch\n");
        for (;;) asm("hlt");
    }

    for (size_t i = 0; i < num_records; i++) {
        record_t *record = &records[i];

        if (record->fn == fn) {
            record->total += time;
            record->calls += 1;
            ri(state);
            return;
        }
    }

    if (num_records == MAX_RECORDS) {
        asm("cli");
        prints("\nmax records\n");
        for (;;) asm("hlt");
    }

    record_t *record = &records[num_records++];
    record->fn = fn;
    record->total = time;
    record->calls = 1;

    ri(state);

    uint64_t end = __builtin_ia32_rdtsc();
    time = end - start;

    for (size_t i = 0; i < idx; i++) {
        current_task->frames[i].ptime += time;
    }
}

#endif
