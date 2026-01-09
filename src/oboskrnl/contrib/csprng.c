/*
 * oboskrnl/contrib/csprng.c
 *
 * Copyright (c) 2026 MarcasRealAccount
 */

#include <memmanip.h>

#include <contrib/random.h>

/* Helper functions: */

static inline uint32_t rotr32(uint32_t v, uint8_t s)
{
    return (v >> s) | (v << (32 - s));
}

static void ctr_drbg_instantiate(csprng* ctx);
static void ctr_drbg_reseed(csprng* ctx);
static void ctr_drbg_generate(csprng* ctx, void* output, size_t size);
static void ctr_drbg_update(const void* provided_data, void* key, void* v);

static void aes128_key_expand(const void* key, void* keys);
static void aes_cipher(const void* keys, const void* in, void* out);

size_t csprng_tjec_read_entropy(void* userdata, void* data, size_t size)
{
    int64_t res = tjec_read_entropy_safe((tjec*) userdata, data, size);
    return res > 0 ? (size_t) res : 0;
}

uint32_t csprng_init(csprng* ctx, const csprng_callbacks* callbacks, uint64_t flags)
{
    if (!ctx || !callbacks || !callbacks->read_entropy)
        return CSPRNG_EINVAL;

    memset(ctx, 0, sizeof(csprng));
    
    ctx->mtx = MUTEX_INITIALIZE();
    memcpy(&ctx->callbacks, callbacks, sizeof(csprng_callbacks));
    ctx->flags = flags;

    ctr_drbg_instantiate(ctx);

    return CSPRNG_ENOERR;
}

void csprng_destroy(csprng* ctx)
{
    if (!ctx)
        return;

    memset(ctx, 0, sizeof(csprng));
}

int64_t csprng_read_random(csprng* ctx, void* data, size_t size)
{
    if (!ctx || !data)
        return 0;

    size_t   orig_size = size;
    uint8_t* ptr       = (uint8_t*) data;

    while (size > 0)
    {
        size_t to_generate = size < 4096 ? size : 4096;
        Core_MutexAcquire(&ctx->mtx);
        ctr_drbg_generate(ctx, ptr, to_generate);
        Core_MutexRelease(&ctx->mtx);
        ptr  += to_generate;
        size -= to_generate;
    }
    return (int64_t) orig_size;
}

// CTR DRBG

void ctr_drbg_instantiate(csprng* ctx)
{
    memset(ctx->v, 0, sizeof(ctx->v));
    memset(ctx->key, 0, sizeof(ctx->key));
    ctr_drbg_reseed(ctx);
}

void ctr_drbg_reseed(csprng* ctx)
{
    uint8_t seed_material[32];
    size_t  read = ctx->callbacks.read_entropy(ctx->callbacks.userdata, seed_material, sizeof(seed_material));
    (void) read;

    ctr_drbg_update(seed_material, ctx->key, ctx->v);
    ctx->reseed_counter = 1;
    
    memset(seed_material, 0, sizeof(seed_material));
}

void ctr_drbg_generate(csprng* ctx, void* output, size_t size)
{
    const uint64_t reseed_interval = 16;

    uint8_t temp[32];

    if (ctx->reseed_counter > reseed_interval)
        ctr_drbg_reseed(ctx);

    uint8_t keys[16*11];
    aes128_key_expand(ctx->key, keys);
    uint8_t* p_out = (uint8_t*) output;

    while (size > 0)
    {
        if (*(uint64_t*) ctx->v == ~((uint64_t)0))
        {
            ((uint64_t*) ctx->v)[0] = 0;
            ++((uint64_t*) ctx->v)[1];
        }
        else
        {
            ++((uint64_t*) ctx->v)[0];
        }
        aes_cipher(keys, ctx->v, temp);
        size_t to_copy = size < 16 ? size : 16;
        memcpy(p_out, temp, to_copy);
        p_out += to_copy;
        size  -= to_copy;
    }
    memset(keys, 0, sizeof(keys));
    memset(temp, 0, sizeof(temp));
    ctr_drbg_update(temp, ctx->key, ctx->v);
    ++ctx->reseed_counter;
}

void ctr_drbg_update(const void* provided_data, void* key, void* v)
{
    uint8_t keys[16*11];
    aes128_key_expand(key, keys);
    aes_cipher(keys, v, key);
    if (*(uint64_t*) v == ~((uint64_t)0))
    {
        ((uint64_t*) v)[0] = 0;
        ++((uint64_t*) v)[1];
    }
    else
    {
        ++((uint64_t*) v)[0];
    }
    aes_cipher(keys, v, v);
    ((uint64_t*) key)[0] ^= ((uint64_t*) provided_data)[0];
    ((uint64_t*) key)[1] ^= ((uint64_t*) provided_data)[1];
    ((uint64_t*) v)[0]   ^= ((uint64_t*) provided_data)[2];
    ((uint64_t*) v)[1]   ^= ((uint64_t*) provided_data)[3];
    memset(keys, 0, sizeof(keys));
}

// AES-128

static const _Alignas(64) uint8_t SBOX_MAT[16][16] = {
	{ 0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76 },
	{ 0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0 },
	{ 0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15 },
	{ 0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75 },
	{ 0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84 },
	{ 0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF },
	{ 0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8 },
	{ 0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2 },
	{ 0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73 },
	{ 0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB },
	{ 0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79 },
	{ 0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08 },
	{ 0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A },
	{ 0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E },
	{ 0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF },
	{ 0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16 }
};
static const uint8_t* SBOX = (const uint8_t*) SBOX_MAT;

typedef union aes_block {
    uint8_t  bytes[16];
    uint32_t words[4];
    uint64_t quads[2];
} aes_block;

void aes128_key_expand(const void* key, void* keys)
{
    const uint8_t Rcon[10] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36 };
    uint8_t* p_bytes = (uint8_t*) keys;
    memcpy(p_bytes, key, 16);
    for (uint8_t i = 0; i < 10; ++i)
    {
		p_bytes[16 + i * 16] = p_bytes[0 + i * 16] ^ SBOX[p_bytes[13 + i * 16]] ^ Rcon[i];
		p_bytes[17 + i * 16] = p_bytes[1 + i * 16] ^ SBOX[p_bytes[14 + i * 16]];
		p_bytes[18 + i * 16] = p_bytes[2 + i * 16] ^ SBOX[p_bytes[15 + i * 16]];
		p_bytes[19 + i * 16] = p_bytes[3 + i * 16] ^ SBOX[p_bytes[12 + i * 16]];

		p_bytes[20 + i * 16] = p_bytes[4 + i * 16] ^ p_bytes[16 + i * 16];
		p_bytes[21 + i * 16] = p_bytes[5 + i * 16] ^ p_bytes[17 + i * 16];
		p_bytes[22 + i * 16] = p_bytes[6 + i * 16] ^ p_bytes[18 + i * 16];
		p_bytes[23 + i * 16] = p_bytes[7 + i * 16] ^ p_bytes[19 + i * 16];

		p_bytes[24 + i * 16] = p_bytes[8 + i * 16] ^ p_bytes[20 + i * 16];
		p_bytes[25 + i * 16] = p_bytes[9 + i * 16] ^ p_bytes[21 + i * 16];
		p_bytes[26 + i * 16] = p_bytes[10 + i * 16] ^ p_bytes[22 + i * 16];
		p_bytes[27 + i * 16] = p_bytes[11 + i * 16] ^ p_bytes[23 + i * 16];

		p_bytes[28 + i * 16] = p_bytes[12 + i * 16] ^ p_bytes[24 + i * 16];
		p_bytes[29 + i * 16] = p_bytes[13 + i * 16] ^ p_bytes[25 + i * 16];
		p_bytes[30 + i * 16] = p_bytes[14 + i * 16] ^ p_bytes[26 + i * 16];
		p_bytes[31 + i * 16] = p_bytes[15 + i * 16] ^ p_bytes[27 + i * 16];
    }
}

void aes_cipher(const void* keys, const void* in, void* out)
{
    const aes_block* key_blocks = (const aes_block*) keys;
    aes_block* block_out = (aes_block*) out;
    memcpy(block_out, in, 16);

    block_out->quads[0] ^= key_blocks[0].quads[0];
    block_out->quads[1] ^= key_blocks[0].quads[1];
    for (size_t i = 1; i < 10; ++i)
    {
        for (size_t j = 0; j < 16; ++j)
            block_out->bytes[j] = SBOX[block_out->bytes[j]];

		uint8_t tmp         = block_out->bytes[1];
		block_out->bytes[1]  = block_out->bytes[5];
		block_out->bytes[5]  = block_out->bytes[9];
		block_out->bytes[9]  = block_out->bytes[13];
		block_out->bytes[13] = tmp;
		tmp                 = block_out->bytes[2];
		block_out->bytes[2]  = block_out->bytes[10];
		block_out->bytes[10] = tmp;
		tmp                 = block_out->bytes[6];
		block_out->bytes[6]  = block_out->bytes[14];
		block_out->bytes[14] = tmp;
		tmp                 = block_out->bytes[15];
		block_out->bytes[15] = block_out->bytes[11];
		block_out->bytes[11] = block_out->bytes[7];
		block_out->bytes[7]  = block_out->bytes[3];
		block_out->bytes[3]  = tmp;
        
        for (uint8_t c = 0; c < 4; ++c)
        {
            uint32_t column = block_out->words[c];
            uint32_t column2 = ((column & 0x7F7F7F7F) << 1) ^ (((column >> 7) & 0x01010101) * 0x1B);
            uint32_t column3 = rotr32(column, 8);
            uint32_t column4 = rotr32(column2, 8);
            uint32_t column5 = rotr32(column, 16);
            uint32_t column6 = rotr32(column, 24);

            block_out->words[c] = column2 ^ column3 ^ column4 ^ column5 ^ column6;
        }

        block_out->quads[0] ^= key_blocks[i].quads[0];
        block_out->quads[1] ^= key_blocks[i].quads[1];
    }

    for (size_t j = 0; j < 16; ++j)
        block_out->bytes[j] = SBOX[block_out->bytes[j]];

    uint8_t tmp         = block_out->bytes[1];
    block_out->bytes[1]  = block_out->bytes[5];
    block_out->bytes[5]  = block_out->bytes[9];
    block_out->bytes[9]  = block_out->bytes[13];
    block_out->bytes[13] = tmp;
    tmp                 = block_out->bytes[2];
    block_out->bytes[2]  = block_out->bytes[10];
    block_out->bytes[10] = tmp;
    tmp                 = block_out->bytes[6];
    block_out->bytes[6]  = block_out->bytes[14];
    block_out->bytes[14] = tmp;
    tmp                 = block_out->bytes[15];
    block_out->bytes[15] = block_out->bytes[11];
    block_out->bytes[11] = block_out->bytes[7];
    block_out->bytes[7]  = block_out->bytes[3];
    block_out->bytes[3]  = tmp;

    block_out->quads[0] ^= key_blocks[10].quads[0];
    block_out->quads[1] ^= key_blocks[10].quads[1];
}