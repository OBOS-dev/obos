/*
 * oboskrnl/contrib/random.h
 *
 * Copyright (c) 2026 MarcasRealAccount
 */

#pragma once

#include <int.h>
#include <stdatomic.h>

#include <locks/mutex.h>

typedef struct tjec_sha3
{
	uint16_t r;
	uint16_t rword;
	uint16_t digest_size;
	uint8_t partial[136];
	uint64_t state[25];
	uint64_t msg_len;
} tjec_sha3;

typedef struct tjec_memory
{
    atomic_uchar* memory;
    size_t cell_size;
    size_t size;
    uint64_t flags;
} tjec_memory;

typedef struct tjec
{
    mutex mtx;

    uint32_t health_failure;
    uint16_t base_acc_count;
    uint8_t osr;
    uint8_t apt_base_set  : 1;
    uint8_t fips_enabled  : 1;
    uint8_t random_access : 1;
    uint8_t lag_predictor : 1;
    uint64_t flags;

    uint64_t prev_time;
    uint64_t prev_delta;
    uint64_t prev_delta2;

    uint64_t common_time_gcd;

    const tjec_memory* memory;
    size_t location;

    uint64_t apt_base;
    uint32_t apt_count;
    uint32_t apt_cutoff;
    uint32_t apt_cutoff_permanent;
    uint32_t apt_observations;

    uint32_t rct_count;

    uint32_t lag_global_cutoff;
    uint32_t lag_local_cutoff;
    uint32_t lag_prediction_success_count;
    uint32_t lag_prediction_success_run;
    uint32_t lag_best_predictor;
    uint32_t lag_observations;
    uint64_t lag_scoreboard[8];
    uint64_t lag_delta_history[8];

    tjec_sha3 sha3;
} tjec;

typedef struct csprng_callbacks
{
    void* userdata;
    size_t(* read_entropy)(void* userdata, void* data, size_t size);
} csprng_callbacks;

typedef struct csprng
{
    csprng_callbacks callbacks;
    
    uint64_t flags;
} csprng;

static const uint64_t TJEC_MEM_32KIB            = 1;
static const uint64_t TJEC_MEM_64KIB            = 2;
static const uint64_t TJEC_MEM_128KIB           = 3;
static const uint64_t TJEC_MEM_256KIB           = 4;
static const uint64_t TJEC_MEM_512KIB           = 5;
static const uint64_t TJEC_MEM_1MIB             = 6;
static const uint64_t TJEC_MEM_2MIB             = 7;
static const uint64_t TJEC_MEM_4MIB             = 8;
static const uint64_t TJEC_MEM_8MIB             = 9;
static const uint64_t TJEC_MEM_16MIB            = 10;
static const uint64_t TJEC_MEM_32MIB            = 11;
static const uint64_t TJEC_MEM_64MIB            = 12;
static const uint64_t TJEC_MEM_128MIB           = 13;
static const uint64_t TJEC_MEM_256MIB           = 14;
static const uint64_t TJEC_MEM_512MIB           = 15;
static const uint64_t TJEC_MEM_RANDOM_ACCESS    = 16;

static const uint64_t TJEC_USE_FIPS             = 1;
static const uint64_t TJEC_USE_LAG_PREDICTOR    = 2;
static const uint64_t TJEC_MAX_ACC_LOOP_BITS_1  = UINT64_C(0) << 8;
static const uint64_t TJEC_MAX_ACC_LOOP_BITS_2  = UINT64_C(1) << 8;
static const uint64_t TJEC_MAX_ACC_LOOP_BITS_3  = UINT64_C(2) << 8;
static const uint64_t TJEC_MAX_ACC_LOOP_BITS_4  = UINT64_C(3) << 8;
static const uint64_t TJEC_MAX_ACC_LOOP_BITS_5  = UINT64_C(4) << 8;
static const uint64_t TJEC_MAX_ACC_LOOP_BITS_6  = UINT64_C(5) << 8;
static const uint64_t TJEC_MAX_ACC_LOOP_BITS_7  = UINT64_C(6) << 8;
static const uint64_t TJEC_MAX_ACC_LOOP_BITS_8  = UINT64_C(7) << 8;
static const uint64_t TJEC_MAX_HASH_LOOP_BITS_1 = UINT64_C(0) << 11;
static const uint64_t TJEC_MAX_HASH_LOOP_BITS_2 = UINT64_C(1) << 11;
static const uint64_t TJEC_MAX_HASH_LOOP_BITS_3 = UINT64_C(2) << 11;
static const uint64_t TJEC_MAX_HASH_LOOP_BITS_4 = UINT64_C(3) << 11;
static const uint64_t TJEC_MAX_HASH_LOOP_BITS_5 = UINT64_C(4) << 11;
static const uint64_t TJEC_MAX_HASH_LOOP_BITS_6 = UINT64_C(5) << 11;
static const uint64_t TJEC_MAX_HASH_LOOP_BITS_7 = UINT64_C(6) << 11;
static const uint64_t TJEC_MAX_HASH_LOOP_BITS_8 = UINT64_C(7) << 11;

static const uint32_t TJEC_ENOERR       = 0;
static const uint32_t TJEC_EINVAL       = 1;
static const uint32_t TJEC_ENOMEM       = 2;
static const uint32_t TJEC_ENOTIME      = 3;
static const uint32_t TJEC_ECOARSETIME  = 4;
static const uint32_t TJEC_ENOMONOTONIC = 5;
static const uint32_t TJEC_ERCT         = 6;
static const uint32_t TJEC_EHEALTH      = 7;
static const uint32_t TJEC_ESTUCK       = 8;
static const uint32_t TJEC_EMINVARVAR   = 9;

static const int64_t TJEC_OSR_FAILURE           = -1;
static const int64_t TJEC_RCT_FAILURE           = -2;
static const int64_t TJEC_APT_FAILURE           = -3;
static const int64_t TJEC_LAG_FAILURE           = -4;
static const int64_t TJEC_UNKNOWN_FAILURE       = -5;
static const int64_t TJEC_RCT_FAILURE_PERMANENT = -6;
static const int64_t TJEC_APT_FAILURE_PERMANENT = -7;
static const int64_t TJEC_LAG_FAILURE_PERMANENT = -8;

uint32_t tjec_memory_init(tjec_memory* mem, uint64_t flags);
void     tjec_memory_destroy(tjec_memory* mem);
size_t   tjec_memory_get_size(const tjec_memory* mem);

uint32_t tjec_pre_init(tjec* ec, const tjec_memory* mem, uint64_t flags);
uint32_t tjec_pre_init_ex(tjec* ec, const tjec_memory* mem, uint64_t flags, uint8_t osr);
uint32_t tjec_init(tjec* ec, const tjec_memory* mem, uint64_t flags);
uint32_t tjec_init_ex(tjec* ec, const tjec_memory* mem, uint64_t flags, uint8_t osr);
void     tjec_destroy(tjec* ec);
int64_t  tjec_read_entropy(tjec* ec, void* data, size_t size);
int64_t  tjec_read_entropy_safe(tjec* ec, void* data, size_t size);

static const uint32_t CSPRNG_ENOERR = 0;
static const uint32_t CSPRNG_EINVAL = 1;

size_t csprng_tjec_read_entropy(void* userdata, void* data, size_t size);

uint32_t csprng_init(csprng* ctx, const csprng_callbacks* callbacks, uint64_t flags);
void     csprng_destroy(csprng* ctx);
int64_t  csprng_read_random(csprng* ctx, void* data, size_t size);

uint8_t  random8(void);
uint16_t random16(void);
uint32_t random32(void);
uint64_t random64(void);
bool     random_buffer(void* buffer, size_t size);