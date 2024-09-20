#include <int.h>
#include <klog.h>

#include <irq/timer.h>

#include "rand.h"

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

void mt_seed(uintptr_t s)
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

OBOS_WEAK uintptr_t random_seed()
{
    return CoreS_GetNativeTimerTick();
}