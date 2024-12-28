#if OBOS_ENABLE_PROFILING

// This file, and prof.c were found at
// https://gist.github.com/monkuous/5752282d03995080e99671ecb9969b3f
// with minor additions

#ifndef HYDROGEN_UTIL_PROF_H
#define HYDROGEN_UTIL_PROF_H

#include <stdint.h>

typedef struct {
    void *fn; // function
    void *site; // call site
    uint64_t start; // tsc value when the function was called
    uint64_t ptime; // total time spent in profiler code across the entire call stack below this function, subtracted from runtime when adding to records
} call_frame_t;

#define MAX_FRAMES 64

void prof_start(void);

void prof_stop(void);

void prof_reset(void);

void prof_show(const char *name);

#endif // HYDROGEN_UTIL_PROF_H

#endif
