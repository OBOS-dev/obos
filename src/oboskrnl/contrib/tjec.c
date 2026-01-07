/*
 * oboskrnl/contrib/tjec.c
 *
 * Copyright (c) 2026 MarcasRealAccount
 */

#include <memmanip.h>

#include <contrib/random.h>

#include <irq/timer.h>

#include <mm/alloc.h>
#include <mm/context.h>

#define ENTROPY_SAFETY_FACTOR 64
#define APT_MASK              (UINT64_C(0xFFFFFFFFFFFFFFFF))
#define APT_WINDOW_SIZE       512
#define LAG_HISTORY_SIZE      (sizeof(((tjec*) 0)->lag_delta_history) / sizeof(*((tjec*) 0)->lag_delta_history))
#define LAG_MASK              (LAG_HISTORY_SIZE - 1)
#define LAG_WINDOW_SIZE       (1 << 10)
#define SHA3_UNROLLED         0

#define CELL_SIZE  128
#define CELL_COUNT 512

#define APT_FAILURE_PERMANENT 1
#define RCT_FAILURE_PERMANENT 2
#define LAG_FAILURE_PERMANENT 4
#define APT_FAILURE           8
#define RCT_FAILURE           16
#define LAG_FAILURE           32

/* Helper functions: */

#if __x86_64__
#   include <arch/x86_64/asm_helpers.h>
#endif

static uint64_t temp_time()
{
#if __x86_64__
    return rdtsc();
#else
    return CoreS_GetNativeTimerTick();
#endif
}

static uint64_t temp_l1_cache_size()
{
#if __x86_64__
    uint32_t ecx;
    uint32_t edx;
    __cpuid__(0x80000005, 0, NULL, NULL, &ecx, &edx);
    uint64_t cachesize = ((uint64_t)((ecx >> 24) & 7) + ((edx >>24 ) & 7)) << 10;
    return cachesize;
#else
    return 0;
#endif
}

static inline uint64_t rotl64(uint64_t v, uint8_t s)
{
    return (v << s) | (v >> (64 - s));
}

static inline uint32_t rotl32(uint32_t v, uint8_t s)
{
    return (v << s) | (v >> (32 - s));
}

static inline uint64_t gcd64(uint64_t a, uint64_t b)
{
    if (a < b)
    {
        uint64_t tmp = a;
        a            = b;
        b            = tmp;
    }

    while (b != 0)
    {
        uint64_t r = a % b;
        a          = b;
        b          = r;
    }
    return a;
}

static void sha3_init(tjec_sha3* ctx);
static void sha3_256_init(tjec_sha3* ctx);
static void sha3_update(tjec_sha3* ctx, const void* data, size_t size);
static void sha3_final(tjec_sha3* ctx, void* digest);

static void apt_init(tjec* ec, uint8_t osr);
static void apt_reinit(tjec* ec, uint64_t delta, uint32_t apt_count, uint32_t apt_observations);
static void apt_reset(tjec* ec);
static void apt_insert(tjec* ec, uint64_t delta);

static void     lag_init(tjec* ec, uint8_t osr);
static void     lag_reset(tjec* ec);
static void     lag_insert(tjec* ec, uint64_t delta);
static uint64_t lag_delta2(tjec* ec, uint64_t delta);
static uint64_t lag_delta3(tjec* ec, uint64_t delta2);

static uint32_t rct_permanent_cutoff(uint8_t osr);
static uint32_t rct_intermittent_cutoff(uint8_t osr);
static void     rct_insert(tjec* ec, int stuck);

static int      tjec_is_stuck(tjec* ec, uint64_t delta);
static uint32_t tjec_health_failure(tjec* ec);
static uint64_t tjec_loop_shuffle(tjec* ec, uint32_t bits, uint32_t min);
static void     tjec_hash_time(tjec* ec, uint64_t delta, int stuck);
static void     tjec_random_memory_access(tjec* ec);
static void     tjec_memory_access(tjec* ec);
static int      tjec_measure_jitter(tjec* ec, uint64_t* current_delta);
static void     tjec_random_data(tjec* ec);
static void     tjec_read_random_block(tjec* ec, void* dst, size_t size);
static uint32_t tjec_find_common_time_gcd(tjec* ec);
static uint64_t tjec_memory_size(uint64_t flags);

static uint64_t tjec_common_time_gcd = 1;

uint32_t tjec_memory_init(tjec_memory* mem, uint64_t flags)
{
    if (!mem)
        return TJEC_EINVAL;

    memset(mem, 0, sizeof(tjec_memory));

    uint64_t memory_size = tjec_memory_size(flags);

    mem->memory    = (atomic_uchar*) Mm_QuickVMAllocate(memory_size, true);
    mem->cell_size = CELL_SIZE;
    mem->size      = memory_size;
    mem->flags     = flags;
    if (!mem->memory)
        return TJEC_ENOMEM;
    memset(mem->memory, 0, memory_size);

    return TJEC_ENOERR;
}

void tjec_memory_destroy(tjec_memory* mem)
{
    if (!mem)
        return;

    Mm_VirtualMemoryFree(&Mm_KernelContext, mem->memory, mem->size);
    memset(mem, 0, sizeof(tjec_memory));
}

size_t tjec_memory_get_size(const tjec_memory* mem)
{
    return mem ? mem->size : 0;
}

uint32_t tjec_pre_init(tjec* ec, const tjec_memory* mem, uint64_t flags)
{
    return tjec_pre_init_ex(ec, mem, flags, 1);
}

uint32_t tjec_pre_init_ex(tjec* ec, const tjec_memory* mem, uint64_t flags, uint8_t osr)
{
    uint32_t err = tjec_init_ex(ec, mem, flags, osr);
    if (err)
        return err;

    err = tjec_find_common_time_gcd(ec);
    if (err)
        return err;

    tjec_common_time_gcd = ec->common_time_gcd;
    return TJEC_ENOERR;
}

uint32_t tjec_init(tjec* ec, const tjec_memory* mem, uint64_t flags)
{
    return tjec_init_ex(ec, mem, flags, 1);
}

uint32_t tjec_init_ex(tjec* ec, const tjec_memory* mem, uint64_t flags, uint8_t osr)
{
    if (!ec || !mem || !osr)
        return TJEC_EINVAL;

    memset(ec, 0, sizeof(tjec));

    ec->mtx             = MUTEX_INITIALIZE();
    ec->base_acc_count  = 64;
    ec->osr             = osr;
    ec->fips_enabled    = !!(flags & TJEC_USE_FIPS);
    ec->random_access   = !!(mem->flags & TJEC_MEM_RANDOM_ACCESS);
    ec->lag_predictor   = !!(flags & TJEC_USE_LAG_PREDICTOR);
    ec->flags           = flags;
    ec->common_time_gcd = tjec_common_time_gcd;
    ec->memory          = mem;

    sha3_256_init(&ec->sha3);
    apt_init(ec, osr);
    if (ec->lag_predictor)
        lag_init(ec, osr);

    tjec_measure_jitter(ec, NULL);

    return TJEC_ENOERR;
}

void tjec_destroy(tjec* ec)
{
    if (!ec)
        return;

    memset(ec, 0, sizeof(tjec));
}

int64_t tjec_read_entropy(tjec* ec, void* data, size_t size)
{
    if (!ec || !data)
        return 0;

    size_t orig_size = size;
    int32_t ret      = 0;

    uint8_t* ptr = (uint8_t*) data;

    uint8_t reacquire_counter = 0;
    Core_MutexAcquire(&ec->mtx);

    while (size > 0)
    {
        tjec_random_data(ec);

        uint32_t health_test = tjec_health_failure(ec);
        if (health_test)
        {
            if (health_test & RCT_FAILURE_PERMANENT)
                ret = TJEC_RCT_FAILURE_PERMANENT;
            else if (health_test & APT_FAILURE_PERMANENT)
                ret = TJEC_APT_FAILURE_PERMANENT;
            else if (health_test & LAG_FAILURE_PERMANENT)
                ret = TJEC_LAG_FAILURE_PERMANENT;
            else if (health_test & RCT_FAILURE)
                ret = TJEC_RCT_FAILURE;
            else if (health_test & APT_FAILURE)
                ret = TJEC_APT_FAILURE;
            else if (health_test & LAG_FAILURE)
                ret = TJEC_LAG_FAILURE;
            else
                ret = TJEC_UNKNOWN_FAILURE;
            break;
        }

        size_t to_copy = size >= 32 ? 32 : size;
        tjec_read_random_block(ec, ptr, to_copy);
        size -= to_copy;
        ptr  += to_copy;

        if (++reacquire_counter >= 128)
        {
            reacquire_counter = 0;
            Core_MutexRelease(&ec->mtx);
            Core_MutexAcquire(&ec->mtx);
        }
    }

    tjec_read_random_block(ec, NULL, 0);

    Core_MutexRelease(&ec->mtx);
    
    return ret ? ret : (int64_t) orig_size;
}

int64_t tjec_read_entropy_safe(tjec* ec, void* data, size_t size)
{
    if (!ec)
        return 0;

    size_t   orig_size = size;
    uint8_t* ptr       = (uint8_t*) data;

    while (size > 0)
    {
        int64_t ret = tjec_read_entropy(ec, ptr, size);

        switch (ret)
        {
        case TJEC_OSR_FAILURE:
        case TJEC_RCT_FAILURE_PERMANENT:
        case TJEC_APT_FAILURE_PERMANENT:
        case TJEC_LAG_FAILURE_PERMANENT:
            return ret;

        case TJEC_UNKNOWN_FAILURE:
        case TJEC_RCT_FAILURE:
        case TJEC_APT_FAILURE:
        case TJEC_LAG_FAILURE:
        {
            Core_MutexAcquire(&ec->mtx);
            uint32_t apt_observations             = ec->apt_observations;
            uint64_t current_delta                = ec->apt_base;
            uint32_t lag_prediction_success_run   = ec->lag_prediction_success_run;
            uint32_t lag_prediction_success_count = ec->lag_prediction_success_count;
            
            const tjec_memory* mem = ec->memory;
            
            uint16_t osr   = (uint16_t) ec->osr + 1;
            uint64_t flags = ec->flags;
            
            if (osr > 20)
                return ret;
                
            while (tjec_init_ex(ec, mem, flags, (uint8_t) osr))
            {
                ++osr;
                if (osr > 20)
                {
                    Core_MutexRelease(&ec->mtx);
                    return TJEC_OSR_FAILURE;
                }
            }
            
            if (apt_observations)
            {
                apt_reinit(ec, current_delta, 0, apt_observations);
                ec->rct_count                    = rct_intermittent_cutoff(osr);
                ec->lag_prediction_success_run   = lag_prediction_success_run;
                ec->lag_prediction_success_count = lag_prediction_success_count;
            }
            Core_MutexRelease(&ec->mtx);
            break;
        }
        default:
            size -= (size_t) ret;
            ptr  += (size_t) ret;
            break;
        }
    }

    return (int64_t) orig_size;
}

// APT

void apt_init(tjec* ec, uint8_t osr)
{
    const uint32_t cutoff_lut[]           = { 325, 422, 459, 477, 488, 494, 499, 502, 505, 507, 508, 509, 510, 511, 512 };
    const uint32_t cutoff_permanent_lut[] = { 355, 447, 479, 494, 502, 507, 510, 512, 512, 512, 512, 512, 512, 512, 512 };
    
    osr = osr < sizeof(cutoff_lut) / sizeof(*cutoff_lut) ? osr : sizeof(cutoff_lut) / sizeof(*cutoff_lut);
    
    ec->apt_cutoff           = cutoff_lut[osr];
    ec->apt_cutoff_permanent = cutoff_permanent_lut[osr];
}

void apt_reinit(tjec* ec, uint64_t delta, uint32_t apt_count, uint32_t apt_observations)
{
    ec->apt_base     = delta;
    ec->apt_base_set = 1;
    if (apt_count)
        ec->apt_count = apt_count;
    else
        ec->apt_count = ec->apt_cutoff;
    ec->apt_observations = apt_observations;
}

void apt_reset(tjec* ec)
{
    ec->apt_base_set = 0;
}

void apt_insert(tjec* ec, uint64_t delta)
{
    delta &= APT_MASK;

    if (!ec->apt_base_set)
    {
        apt_reinit(ec, delta, 1, 1);
        return;
    }

    if (delta == ec->apt_base)
    {
        ++ec->apt_count;

        if (ec->apt_count >= ec->apt_cutoff_permanent)
            ec->health_failure |= APT_FAILURE_PERMANENT;
        else if (ec->apt_count == ec->apt_cutoff)
            ec->health_failure |= APT_FAILURE;
    }

    ++ec->apt_observations;

    if (ec->apt_observations >= APT_WINDOW_SIZE)
        apt_reset(ec);
}

// LAG

void lag_init(tjec* ec, uint8_t osr)
{
    const uint32_t global_cutoff_lut[] = { 66443, 93504, 104761, 110875, 114707, 117330, 119237, 120686, 121823, 122739, 123493, 124124, 124660, 125120, 125520, 125871, 126181, 126457, 126704, 126926 };
    const uint32_t local_cutoff_lut[]  = { 38, 75, 111, 146, 181, 215, 250, 284, 318, 351, 385, 419, 452, 485, 518, 551, 584, 617, 650, 683 };

    osr = osr < sizeof(global_cutoff_lut) / sizeof(*global_cutoff_lut) ? osr : sizeof(global_cutoff_lut) / sizeof(*global_cutoff_lut);

    ec->lag_global_cutoff = global_cutoff_lut[osr - 1];
    ec->lag_local_cutoff  = local_cutoff_lut[osr - 1];
}

void lag_reset(tjec* ec)
{
    ec->lag_prediction_success_count = 0;
    ec->lag_prediction_success_run   = 0;
    ec->lag_best_predictor           = 0;
    ec->lag_observations             = 0;

    for (uint32_t i = 0; i < LAG_HISTORY_SIZE; ++i)
    {
        ec->lag_scoreboard[i]    = 0;
        ec->lag_delta_history[i] = 0;
    }
}

#define LAG_HISTORY(ec, loc) (ec->lag_delta_history[(ec->lag_observations - loc - 1) & LAG_MASK])

void lag_insert(tjec* ec, uint64_t delta)
{
    if (ec->lag_observations < LAG_HISTORY_SIZE)
    {
        ec->lag_delta_history[ec->lag_observations++] = delta;
        return;
    }

    uint64_t prediction = LAG_HISTORY(ec, ec->lag_best_predictor);
    if (prediction == delta)
    {
        ++ec->lag_prediction_success_count;
        ++ec->lag_prediction_success_run;

        if ((ec->lag_prediction_success_run >= ec->lag_local_cutoff) ||
            (ec->lag_prediction_success_count >= ec->lag_global_cutoff))
            ec->health_failure |= LAG_FAILURE;
    }
    else
    {
        ec->lag_prediction_success_run = 0;
    }

    for (uint32_t i = 0; i < LAG_HISTORY_SIZE; ++i)
    {
        if (LAG_HISTORY(ec, i) == delta)
        {
            ++ec->lag_scoreboard[i];

            if (ec->lag_scoreboard[i] > ec->lag_scoreboard[ec->lag_best_predictor])
                ec->lag_best_predictor = i;
        }
    }

    ec->lag_delta_history[(ec->lag_observations++) & LAG_MASK] = delta;
    if (ec->lag_observations >= LAG_WINDOW_SIZE)
        lag_reset(ec);
}

uint64_t lag_delta2(tjec* ec, uint64_t delta)
{
    return delta - LAG_HISTORY(ec, 0);
}

uint64_t lag_delta3(tjec* ec, uint64_t delta2)
{
    return delta2 - (LAG_HISTORY(ec, 0) - LAG_HISTORY(ec, 1));
}

// RCT

uint32_t rct_permanent_cutoff(uint8_t osr)
{
    return osr * 60;
}

uint32_t rct_intermittent_cutoff(uint8_t osr)
{
    return osr * 30;
}

void rct_insert(tjec* ec, int stuck)
{
    if (stuck)
    {
        ++ec->rct_count;

        if (ec->rct_count >= rct_permanent_cutoff(ec->osr))
            ec->health_failure |= RCT_FAILURE_PERMANENT;
        else if (ec->rct_count >= rct_intermittent_cutoff(ec->osr))
            ec->health_failure |= RCT_FAILURE;
    }
    else
    {
        ec->rct_count = 0;
    }
}

// TJEC

int tjec_is_stuck(tjec* ec, uint64_t delta)
{
    uint64_t delta2;
    uint64_t delta3;
    if (ec->lag_predictor)
    {
        delta2 = lag_delta2(ec, delta);
        delta3 = lag_delta3(ec, delta2);
    }
    else
    {
        delta2          = delta - ec->prev_delta;
        ec->prev_delta  = delta;
        delta3          = delta2 - ec->prev_delta2;
        ec->prev_delta2 = delta2;
    }

    apt_insert(ec, delta);
    if (ec->lag_predictor)
        lag_insert(ec, delta);

    if (!delta || !delta2 || !delta3)
    {
        rct_insert(ec, 1);
        return 1;
    }

    rct_insert(ec, 0);
    return 0;
}

uint32_t tjec_health_failure(tjec* ec)
{
    if (!ec->fips_enabled)
        return 0;
    return ec->health_failure;
}

uint64_t tjec_loop_shuffle(tjec* ec, uint32_t bits, uint32_t min)
{
    uint64_t time_now = (uint64_t) temp_time();

    uint64_t mask = (UINT64_C(1) << bits) - 1;
    uint64_t shuffle = 0;
    for (uint32_t i = 0; i < (((sizeof(time_now) << 3) + bits - 1) / bits); ++i)
    {
        shuffle ^= time_now & mask;
        time_now = time_now >> bits;
    }
    return shuffle + (UINT64_C(1) << min);
}

void tjec_hash_time(tjec* ec, uint64_t delta, int stuck)
{
    tjec_sha3 sha3;

    uint8_t temp[sizeof(((tjec_sha3*) 0)->partial)];
    memset(temp, 0, sizeof(temp));

    uint64_t loop_count = tjec_loop_shuffle(ec, 1 + ((ec->flags >> 11) & 7), 0);

    sha3_256_init(&sha3);
    for (uint64_t j = 0; j < loop_count; ++j)
    {
        sha3_update(&sha3, temp, 32);
        sha3_update(&sha3, &ec->rct_count, sizeof(ec->rct_count));
        sha3_update(&sha3, &ec->apt_cutoff, sizeof(ec->apt_cutoff));
        sha3_update(&sha3, &ec->apt_observations, sizeof(ec->apt_observations));
        sha3_update(&sha3, &ec->apt_count, sizeof(ec->apt_count));
        sha3_update(&sha3, &ec->apt_base, sizeof(ec->apt_base));
        sha3_update(&sha3, &j, sizeof(j));
        sha3_final(&sha3, temp);
    }

    if (stuck)
        delta = 0;

    memcpy(temp + 32, &delta, sizeof(delta));
    sha3_update(&ec->sha3, temp, sizeof(temp));

    memset(&sha3, 0, sizeof(sha3));
    memset(temp, 0, sizeof(temp));
}

static uint32_t xoshiro128starstar(uint32_t* s)
{
    const uint32_t result = rotl32(s[1] * 5, 7) * 9;
    const uint32_t t      = s[1] << 9;

    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3]  = rotl32(s[3], 11);
    return result;
}

void tjec_random_memory_access(tjec* ec)
{
    uint32_t prng_state[4] = { 0x8E93EEC0, 0xCE65608A, 0xA8D46B46, 0xE83CEF69 };

    uint64_t loop_count = ec->base_acc_count + tjec_loop_shuffle(ec, 1 + ((ec->flags >> 8) & 7), 0);

    for (size_t i = 0; i < sizeof(prng_state) / sizeof(*prng_state); ++i)
    {
        prng_state[i] = (temp_time() & 0xFF) | ((temp_time() & 0xFF) << 8) | ((temp_time() & 0xFF) << 16) | ((temp_time() & 0xFF) << 24);
    }

    for (uint64_t i = 0; i < loop_count; ++i)
    {
        atomic_uchar* pMem = ec->memory->memory + (xoshiro128starstar(prng_state) % ec->memory->size);
        *pMem              = *pMem + 1;
    }
}

void tjec_memory_access(tjec* ec)
{
    uint64_t loop_count = ec->base_acc_count + tjec_loop_shuffle(ec, 1 + ((ec->flags >> 8) & 7), 0);

    for (uint64_t i = 0; i < loop_count; ++i)
    {
        atomic_uchar* pMem = ec->memory->memory + ec->location;
        *pMem              = *pMem + 1;

        ec->location = ec->location + ec->memory->cell_size - 1;
        ec->location = ec->location % ec->memory->size;
    }
}

int tjec_measure_jitter(tjec* ec, uint64_t* current_delta)
{
    if (ec->random_access)
        tjec_random_memory_access(ec);
    else
        tjec_memory_access(ec);

    uint64_t time_now = temp_time();
    uint64_t delta    = (time_now - ec->prev_time) / ec->common_time_gcd;
    ec->prev_time     = time_now;

    int stuck = tjec_is_stuck(ec, delta);
    tjec_hash_time(ec, delta, stuck);

    if (current_delta)
        *current_delta = delta;
    return stuck;
}

void tjec_random_data(tjec* ec)
{
    uint32_t bits_to_read = (256 + (ec->fips_enabled ? ENTROPY_SAFETY_FACTOR : 0)) * ec->osr;

    tjec_measure_jitter(ec, NULL);

    uint32_t k = 0;
    while (!tjec_health_failure(ec))
    {
        if (tjec_measure_jitter(ec, NULL))
            continue;

        if (++k >= bits_to_read)
            break;
    }
}

void tjec_read_random_block(tjec* ec, void* dst, size_t size)
{
    uint8_t block[32];
    sha3_final(&ec->sha3, block);
    if (size)
        memcpy(dst, block, size);
    sha3_update(&ec->sha3, block, sizeof(block));
    memset(block, 0, sizeof(block));
}

uint32_t tjec_find_common_time_gcd(tjec* ec)
{
    uint64_t gcd_history[1024];
    uint32_t time_backwards = 0;
    uint32_t count_stuck    = 0;

    const int CLEARCACHE = 100;
    for (int i = -CLEARCACHE; i < 1024; ++i)
    {
        uint64_t delta      = 0;
        int      stuck      = tjec_measure_jitter(ec, &delta);
        uint64_t end_time   = ec->prev_time;
        uint64_t start_time = end_time - delta;
        if (!start_time || !end_time)
            return TJEC_ENOTIME;

        if (!delta || (end_time == start_time))
            return TJEC_ECOARSETIME;

        if (i < 0)
            continue;

        if (stuck)
            ++count_stuck;

        if (end_time < start_time)
            ++time_backwards;

        gcd_history[i] = delta;
    }

    if (time_backwards > 3)
        return TJEC_ENOMONOTONIC;

    uint32_t health_test = tjec_health_failure(ec);
    if (health_test)
        return (health_test & RCT_FAILURE) ? TJEC_ERCT : TJEC_EHEALTH;

    if (count_stuck > (1024 * 9) / 10)
        return TJEC_ESTUCK;

    {
        uint64_t running_gcd = gcd_history[0];
        uint64_t delta_sum   = 0;

        for (size_t i = 1; i < 1024; ++i)
        {
            if (gcd_history[i] >= gcd_history[i - 1])
                delta_sum += gcd_history[i] - gcd_history[i - 1];
            else
                delta_sum += gcd_history[i - 1] - gcd_history[i];
            running_gcd = gcd64(gcd_history[i], running_gcd);
        }

        if ((delta_sum * ec->osr) < 1024)
            return TJEC_EMINVARVAR;
        if (running_gcd >= UINT32_MAX / 2)
            return TJEC_ECOARSETIME;

        ec->common_time_gcd = running_gcd;
    }

    return TJEC_ENOERR;
}

uint64_t tjec_memory_size(uint64_t flags)
{
    uint64_t max_memory_size;
    if ((flags & 0xF) == 0)
    {
        if (flags & TJEC_MEM_RANDOM_ACCESS)
            max_memory_size = UINT64_C(1) << 17;
        else
            max_memory_size = CELL_SIZE * CELL_COUNT;
    }
    else
    {
        max_memory_size = UINT64_C(32) << (9 + (flags & 0xF));
    }

    uint64_t cache_memory_size = temp_l1_cache_size();
    uint64_t memory_size       = (!cache_memory_size || max_memory_size < cache_memory_size) ? max_memory_size : cache_memory_size;
    return memory_size;
}

// SHA-3

#if SHA3_UNROLLED

static void keccakp_1600(uint64_t* s)
{
    const uint8_t  rols[24] = { 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14 };
    const uint64_t iota_values[] = { 0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL, 0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL };

    uint64_t C[5], D[5];
    for (uint8_t round = 0; round < 24; ++round)
    {
        C[0]   = s[0] ^ s[5] ^ s[10] ^ s[15] ^ s[20];
        C[1]   = s[1] ^ s[6] ^ s[11] ^ s[16] ^ s[21];
        C[2]   = s[2] ^ s[7] ^ s[12] ^ s[17] ^ s[22];
        C[3]   = s[3] ^ s[8] ^ s[13] ^ s[18] ^ s[23];
        C[4]   = s[4] ^ s[9] ^ s[14] ^ s[19] ^ s[24];
        D[0]   = C[4] ^ rotl64(C[1], 1);
        D[1]   = C[0] ^ rotl64(C[2], 1);
        D[2]   = C[1] ^ rotl64(C[3], 1);
        D[3]   = C[2] ^ rotl64(C[4], 1);
        D[4]   = C[3] ^ rotl64(C[0], 1);

        uint64_t tmp = s[7];
        
        s[0]  = s[0] ^ D[0];
        s[7]  = rotl64(s[10] ^ D[0], rols[9]);
        s[10] = rotl64(s[1] ^ D[1], rols[0]);
        s[1]  = rotl64(s[6] ^ D[1], rols[5]);
        s[6]  = rotl64(s[9] ^ D[4], rols[8]);
        s[9]  = rotl64(s[22] ^ D[2], rols[21]);
        s[22] = rotl64(s[14] ^ D[4], rols[13]);
        s[14] = rotl64(s[20] ^ D[0], rols[19]);
        s[20] = rotl64(s[2] ^ D[2], rols[1]);
        s[2]  = rotl64(s[12] ^ D[2], rols[11]);
        s[12] = rotl64(s[13] ^ D[3], rols[12]);
        s[13] = rotl64(s[19] ^ D[4], rols[18]);
        s[19] = rotl64(s[23] ^ D[3], rols[22]);
        s[23] = rotl64(s[15] ^ D[0], rols[14]);
        s[15] = rotl64(s[4] ^ D[4], rols[3]);
        s[4]  = rotl64(s[24] ^ D[4], rols[23]);
        s[24] = rotl64(s[21] ^ D[1], rols[20]);
        s[21] = rotl64(s[8] ^ D[3], rols[7]);
        s[8]  = rotl64(s[16] ^ D[1], rols[15]);
        s[16] = rotl64(s[5] ^ D[0], rols[4]);
        s[5]  = rotl64(s[3] ^ D[3], rols[2]);
        s[3]  = rotl64(s[18] ^ D[3], rols[17]);
        s[18] = rotl64(s[17] ^ D[2], rols[16]);
        s[17] = rotl64(s[11] ^ D[1], rols[10]);
        s[11] = rotl64(tmp ^ D[2], rols[6]);

        C[0]  = s[0];
        C[1]  = s[5];
        C[2]  = s[10];
        C[3]  = s[15];
        C[4]  = s[20];
        D[0]  = s[1];
        D[1]  = s[6];
        D[2]  = s[11];
        D[3]  = s[16];
        D[4]  = s[21];
        s[0]  ^= ~s[1] & s[2];
        s[1]  ^= ~s[2] & s[3];
        s[2]  ^= ~s[3] & s[4];
        s[3]  ^= ~s[4] & C[0];
        s[4]  ^= ~C[0] & D[0];
        s[5]  ^= ~s[6] & s[7];
        s[6]  ^= ~s[7] & s[8];
        s[7]  ^= ~s[8] & s[9];
        s[8]  ^= ~s[9] & C[1];
        s[9]  ^= ~C[1] & D[1];
        s[10] ^= ~s[11] & s[12];
        s[11] ^= ~s[12] & s[13];
        s[12] ^= ~s[13] & s[14];
        s[13] ^= ~s[14] & C[2];
        s[14] ^= ~C[2] & D[2];
        s[15] ^= ~s[16] & s[17];
        s[16] ^= ~s[17] & s[18];
        s[17] ^= ~s[18] & s[19];
        s[18] ^= ~s[19] & C[3];
        s[19] ^= ~C[3] & D[3];
        s[20] ^= ~s[21] & s[22];
        s[21] ^= ~s[22] & s[23];
        s[22] ^= ~s[23] & s[24];
        s[23] ^= ~s[24] & C[4];
        s[24] ^= ~C[4] & D[4];

        s[0] ^= iota_values[round];
    }
}

#else

static inline void keccakp_theta(uint64_t* s)
{
	uint64_t C[5], D[5];
	C[0]   = s[0] ^ s[5] ^ s[10] ^ s[15] ^ s[20];
	C[1]   = s[1] ^ s[6] ^ s[11] ^ s[16] ^ s[21];
	C[2]   = s[2] ^ s[7] ^ s[12] ^ s[17] ^ s[22];
	C[3]   = s[3] ^ s[8] ^ s[13] ^ s[18] ^ s[23];
	C[4]   = s[4] ^ s[9] ^ s[14] ^ s[19] ^ s[24];
	D[0]   = C[4] ^ rotl64(C[1], 1);
	D[1]   = C[0] ^ rotl64(C[2], 1);
	D[2]   = C[1] ^ rotl64(C[3], 1);
	D[3]   = C[2] ^ rotl64(C[4], 1);
	D[4]   = C[3] ^ rotl64(C[0], 1);
	s[0]  ^= D[0];
	s[1]  ^= D[1];
	s[2]  ^= D[2];
	s[3]  ^= D[3];
	s[4]  ^= D[4];
	s[5]  ^= D[0];
	s[6]  ^= D[1];
	s[7]  ^= D[2];
	s[8]  ^= D[3];
	s[9]  ^= D[4];
	s[10] ^= D[0];
	s[11] ^= D[1];
	s[12] ^= D[2];
	s[13] ^= D[3];
	s[14] ^= D[4];
	s[15] ^= D[0];
	s[16] ^= D[1];
	s[17] ^= D[2];
	s[18] ^= D[3];
	s[19] ^= D[4];
	s[20] ^= D[0];
	s[21] ^= D[1];
	s[22] ^= D[2];
	s[23] ^= D[3];
	s[24] ^= D[4];
}

static inline void keccakp_rho(uint64_t* s)
{
	const uint8_t rols[24] = { 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14 };
	s[1]  = rotl64(s[1], rols[0]);
	s[2]  = rotl64(s[2], rols[1]);
	s[3]  = rotl64(s[3], rols[2]);
	s[4]  = rotl64(s[4], rols[3]);
	s[5]  = rotl64(s[5], rols[4]);
	s[6]  = rotl64(s[6], rols[5]);
	s[7]  = rotl64(s[7], rols[6]);
	s[8]  = rotl64(s[8], rols[7]);
	s[9]  = rotl64(s[9], rols[8]);
	s[10] = rotl64(s[10], rols[9]);
	s[11] = rotl64(s[11], rols[10]);
	s[12] = rotl64(s[12], rols[11]);
	s[13] = rotl64(s[13], rols[12]);
	s[14] = rotl64(s[14], rols[13]);
	s[15] = rotl64(s[15], rols[14]);
	s[16] = rotl64(s[16], rols[15]);
	s[17] = rotl64(s[17], rols[16]);
	s[18] = rotl64(s[18], rols[17]);
	s[19] = rotl64(s[19], rols[18]);
	s[20] = rotl64(s[20], rols[19]);
	s[21] = rotl64(s[21], rols[20]);
	s[22] = rotl64(s[22], rols[21]);
	s[23] = rotl64(s[23], rols[22]);
	s[24] = rotl64(s[24], rols[23]);
}

static inline void keccakp_pi(uint64_t* s)
{
	uint64_t tmp = s[21];
	s[0]         = s[0];
	s[1]         = s[6];
	s[2]         = s[12];
	s[3]         = s[18];
	s[4]         = s[24];
	s[5]         = s[3];
	s[6]         = s[9];
	s[7]         = s[10];
	s[8]         = s[16];
	s[9]         = s[22];
	s[10]        = s[1];
	s[11]        = s[7];
	s[12]        = s[13];
	s[13]        = s[19];
	s[14]        = s[20];
	s[15]        = s[4];
	s[16]        = s[5];
	s[17]        = s[11];
	s[18]        = s[17];
	s[19]        = s[23];
	s[20]        = s[2];
	s[21]        = s[8];
	s[22]        = s[14];
	s[23]        = s[15];
	s[24]        = tmp;
}

static inline void keccakp_chi(uint64_t* s)
{
	uint64_t t0[5], t1[5];
	t0[0]  = s[0];
	t0[1]  = s[5];
	t0[2]  = s[10];
	t0[3]  = s[15];
	t0[4]  = s[20];
	t1[0]  = s[1];
	t1[1]  = s[6];
	t1[2]  = s[11];
	t1[3]  = s[16];
	t1[4]  = s[21];
	s[0]  ^= ~s[1] & s[2];
	s[5]  ^= ~s[6] & s[7];
	s[10] ^= ~s[11] & s[12];
	s[15] ^= ~s[16] & s[17];
	s[20] ^= ~s[21] & s[22];
	s[1]  ^= ~s[2] & s[3];
	s[6]  ^= ~s[7] & s[8];
	s[11] ^= ~s[12] & s[13];
	s[16] ^= ~s[17] & s[18];
	s[21] ^= ~s[22] & s[23];
	s[2]  ^= ~s[3] & s[4];
	s[7]  ^= ~s[8] & s[9];
	s[12] ^= ~s[13] & s[14];
	s[17] ^= ~s[18] & s[19];
	s[22] ^= ~s[23] & s[24];
	s[3]  ^= ~s[4] & t0[0];
	s[8]  ^= ~s[9] & t0[1];
	s[13] ^= ~s[14] & t0[2];
	s[18] ^= ~s[19] & t0[3];
	s[23] ^= ~s[24] & t0[4];
	s[4]  ^= ~t0[0] & t1[0];
	s[9]  ^= ~t0[1] & t1[1];
	s[14] ^= ~t0[2] & t1[2];
	s[19] ^= ~t0[3] & t1[3];
	s[24] ^= ~t0[4] & t1[4];
}

static inline void keccakp_iota(uint64_t* s, uint8_t round)
{
    const uint64_t iota_values[] = { 0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL, 0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL };
	s[0] ^= iota_values[round];
}

static void keccakp_1600(uint64_t* s)
{
	for (uint8_t round = 0; round < 24; ++round)
	{
		keccakp_theta(s);
		keccakp_rho(s);
		keccakp_pi(s);
		keccakp_chi(s);
		keccakp_iota(s, round);
	}
}

#endif

void sha3_init(tjec_sha3* ctx)
{
    memset(ctx->state, 0, sizeof(ctx->state));
    ctx->msg_len = 0;
}

void sha3_256_init(tjec_sha3* ctx)
{
    sha3_init(ctx);
    ctx->r           = 136;
    ctx->rword       = 136 / sizeof(uint64_t);
    ctx->digest_size = 32;
}

void sha3_update(tjec_sha3* ctx, const void* data, size_t size)
{
    const uint8_t* ptr = (const uint8_t*) data;

    size_t partial = ctx->msg_len % ctx->r;
    ctx->msg_len  += size;
    if (partial)
    {
        size_t to_copy = ctx->r - partial;
        if (size < to_copy)
        {
            memcpy(ctx->partial + partial, ptr, size);
            return;
        }

        memcpy(ctx->partial + partial, ptr, to_copy);
        size -= to_copy;
        ptr  += to_copy;

        for (size_t i = 0; i < ctx->rword; ++i)
            ctx->state[i] ^= ((uint64_t*) ctx->partial)[i];
        keccakp_1600(ctx->state);
    }

    for (; size >= ctx->r; size -= ctx->r, ptr += ctx->r)
    {
        for (size_t i = 0; i < ctx->rword; ++i)
            ctx->state[i] ^= ((uint64_t*) ptr)[i];
        keccakp_1600(ctx->state);
    }

    if (size)
        memcpy(ctx->partial, ptr, size);
}

void sha3_final(tjec_sha3* ctx, void* digest)
{
    size_t partial = ctx->msg_len % ctx->r;
    memset(ctx->partial + partial, 0, ctx->r - partial);

    ctx->partial[partial]     = 0x06;
    ctx->partial[ctx->r - 1] |= 0x80;

    for (size_t i = 0; i < ctx->rword; ++i)
        ctx->state[i] ^= ((uint64_t*) ctx->partial)[i];
    keccakp_1600(ctx->state);

    memcpy(digest, ctx->state, ctx->digest_size);
    memset(ctx->partial, 0, ctx->r);
    sha3_init(ctx);
}