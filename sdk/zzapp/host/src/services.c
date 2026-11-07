/*
 * Card services on the host. SHA-256 is implemented here. Decompress, image
 * decode and audio decode return ZZ_ENOSYS: the in-tree decoders are GPL
 * firmware glue, a downloaded third-party tree, or an LHA decoder with its
 * own licence and a FILE* interface. None of those belong in this MIT library.
 * GL is parked; bind returns NULL, which is the documented failure.
 *
 * SPDX-License-Identifier: MIT
 */
#include "host_internal.h"

#include <string.h>

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_block(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t i;

    for (i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)block[i * 4u] << 24) |
               ((uint32_t)block[i * 4u + 1u] << 16) |
               ((uint32_t)block[i * 4u + 2u] << 8) |
               (uint32_t)block[i * 4u + 3u];
    }
    for (i = 16u; i < 64u; i++) {
        uint32_t s0 = rotr(w[i - 15u], 7u) ^ rotr(w[i - 15u], 18u) ^
                      (w[i - 15u] >> 3);
        uint32_t s1 = rotr(w[i - 2u], 17u) ^ rotr(w[i - 2u], 19u) ^
                      (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];
    for (i = 0; i < 64u; i++) {
        uint32_t s1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

static void sha256_bytes(const uint8_t *data, uint32_t bytes, uint8_t digest[32])
{
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    uint8_t block[64];
    uint32_t off = 0;
    uint64_t bits = (uint64_t)bytes * 8u;
    uint32_t i;

    while (bytes - off >= 64u) {
        sha256_block(state, data + off);
        off += 64u;
    }
    memset(block, 0, sizeof block);
    if (bytes > off)
        memcpy(block, data + off, bytes - off);
    block[bytes - off] = 0x80u;
    if (bytes - off >= 56u) {
        sha256_block(state, block);
        memset(block, 0, sizeof block);
    }
    for (i = 0; i < 8u; i++)
        block[63u - i] = (uint8_t)(bits >> (8u * i));
    sha256_block(state, block);
    for (i = 0; i < 8u; i++) {
        digest[i * 4u] = (uint8_t)(state[i] >> 24);
        digest[i * 4u + 1u] = (uint8_t)(state[i] >> 16);
        digest[i * 4u + 2u] = (uint8_t)(state[i] >> 8);
        digest[i * 4u + 3u] = (uint8_t)state[i];
    }
}

static int api_sha256(const void *data, uint32_t bytes, uint8_t digest[32])
{
    if (!data || !digest)
        return ZZ_EINVAL;
    sha256_bytes((const uint8_t *)data, bytes, digest);
    return ZZ_OK;
}

static int api_decompress(uint32_t algorithm, uint32_t flags, const void *src,
                          uint32_t src_bytes, void *dst, uint32_t dst_bytes,
                          zzapp_service_result *out)
{
    (void)algorithm;
    (void)flags;
    (void)src;
    (void)src_bytes;
    (void)dst;
    (void)dst_bytes;
    (void)out;
    return ZZ_ENOSYS;
}

static int api_image_open(const zzapp_image_open *open, uint32_t *session,
                          zzapp_image_result *out)
{
    (void)open;
    (void)session;
    (void)out;
    return ZZ_ENOSYS;
}

static int api_image_decode(uint32_t session, const void *src,
                            uint32_t src_bytes, uint32_t flags,
                            zzapp_image_result *out)
{
    (void)session;
    (void)src;
    (void)src_bytes;
    (void)flags;
    (void)out;
    return ZZ_ENOSYS;
}

static int api_image_close(uint32_t session)
{
    (void)session;
    return ZZ_ENOSYS;
}

static int api_audio_decode_open(uint32_t codec, uint32_t *session)
{
    (void)codec;
    (void)session;
    return ZZ_ENOSYS;
}

static int api_audio_decode(uint32_t session, const void *src,
                            uint32_t src_bytes, uint32_t flags, int16_t *pcm,
                            uint32_t pcm_frames, zzapp_audio_decode_result *out)
{
    (void)session;
    (void)src;
    (void)src_bytes;
    (void)flags;
    (void)pcm;
    (void)pcm_frames;
    (void)out;
    return ZZ_ENOSYS;
}

static int api_audio_decode_close(uint32_t session)
{
    (void)session;
    return ZZ_ENOSYS;
}

static const zzapp_gl *api_gl_bind(uint32_t surface_id)
{
    (void)surface_id;
    return 0;
}

static const zzapp_gl *api_gl_bind_flags(uint32_t surface_id, uint32_t flags)
{
    (void)surface_id;
    (void)flags;
    return 0;
}

static void api_gl_swap(void)
{
}

static void api_gl_unbind(void)
{
}

void host_services_bind(zzapp_api *api)
{
    api->sha256 = api_sha256;
    api->decompress = api_decompress;
    api->image_open = api_image_open;
    api->image_decode = api_image_decode;
    api->image_close = api_image_close;
    api->audio_decode_open = api_audio_decode_open;
    api->audio_decode = api_audio_decode;
    api->audio_decode_close = api_audio_decode_close;
    api->gl_bind = api_gl_bind;
    api->gl_bind_flags = api_gl_bind_flags;
    api->gl_swap = api_gl_swap;
    api->gl_unbind = api_gl_unbind;
}

int host_services_selftest(void)
{
    static const uint8_t abc[] = {'a', 'b', 'c'};
    static const uint8_t expect[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    uint8_t digest[32];

    if (api_sha256(abc, 3u, digest) != ZZ_OK)
        return 1;
    if (memcmp(digest, expect, 32) != 0)
        return 2;
    if (api_decompress(ZZ_COMPRESS_ZLIB, 0, abc, 3u, digest, 32u, 0) !=
        ZZ_ENOSYS)
        return 3;
    if (api_gl_bind(1u) != 0)
        return 4;
    return 0;
}
