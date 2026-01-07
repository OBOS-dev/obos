/*
 * oboskrnl/contrib/csprng.c
 *
 * Copyright (c) 2026 MarcasRealAccount
 */

#include <memmanip.h>

#include <contrib/random.h>

// TODO: Make thread-safe

size_t csprng_tjec_read_entropy(void* userdata, void* data, size_t size)
{
    int64_t res = tjec_read_entropy_safe((tjec*) userdata, data, size);
    return res > 0 ? (size_t) res : 0;
}

uint32_t csprng_init(csprng* ctx, const csprng_callbacks* callbacks, uint64_t flags)
{
    if (!ctx || !callbacks)
        return CSPRNG_EINVAL;

    memset(ctx, 0, sizeof(csprng));

    memcpy(&ctx->callbacks, callbacks, sizeof(csprng_callbacks));
    ctx->flags = flags;

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

    size_t read = ctx->callbacks.read_entropy(ctx->callbacks.userdata, data, size);
    return (int64_t) read;
}