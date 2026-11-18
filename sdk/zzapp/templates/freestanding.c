/*
 * Freestanding template: no newlib. Build it with ZZAPP_FREESTANDING=1
 * (make) or FREESTANDING (CMake). The image links crt0, libzzapp and libgcc
 * only. It brings its own bump allocator over the app heap, formats its own
 * log line, and implements its own hash. The struct copy below compiles to a
 * call to memcpy, which libzzapp provides in this mode.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"
#include "zzapp_note.h"

ZZAPP_NOTE(0, 0)

/* Larger than one heap step, so the template exercises zz_heap_grow. */
#define BUF_BYTES (3u << 20)

#define HEAP_STEP (1u << 20)

static uint8_t *heap_cur;
static uint8_t *heap_end;

static void heap_init(void)
{
    void *base = 0;
    uint32_t bytes = 0;

    zz_heap_range(&base, &bytes);
    heap_cur = (uint8_t *)base;
    heap_end = (uint8_t *)base + bytes;
}

/* Bump allocation. Memory is never freed; the heap goes with the app. */
static void *bump_alloc(uint32_t bytes, uint32_t align)
{
    uintptr_t at;
    uint32_t step;
    void *old_break = 0;

    if (!heap_cur || align == 0u || (align & (align - 1u)) != 0u)
        return 0;
    at = ((uintptr_t)heap_cur + align - 1u) & ~(uintptr_t)(align - 1u);
    if (at + bytes < at)
        return 0;
    while (at + bytes > (uintptr_t)heap_end) {
        step = (uint32_t)((at + bytes - (uintptr_t)heap_end + HEAP_STEP - 1u) &
                          ~(uintptr_t)(HEAP_STEP - 1u));
        /* The table grows the heap at its end, so the new range must start
         * where the old one stopped. */
        if (zz_heap_grow(step, &old_break) != ZZ_OK ||
            (uint8_t *)old_break != heap_end)
            return 0;
        heap_end += step;
    }
    heap_cur = (uint8_t *)at + bytes;
    return (void *)at;
}

static uint32_t fnv1a32(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;

    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

typedef struct {
    uint8_t bytes[256];
} block;

static char *put_str(char *out, const char *s)
{
    while (*s)
        *out++ = *s++;
    return out;
}

static char *put_hex(char *out, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    int shift;

    for (shift = 28; shift >= 0; shift -= 4)
        *out++ = digits[(v >> shift) & 0xfu];
    return out;
}

/* Dividing by 10 calls __aeabi_uidiv from libgcc: the Cortex-A9 has no
 * integer divide instruction. */
static char *put_dec(char *out, uint32_t v)
{
    char tmp[10];
    uint32_t n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);
    while (n)
        *out++ = tmp[--n];
    return out;
}

static void fail(void)
{
    static const char bad[] = "freestanding FAIL\n";

    zz_log(bad, sizeof bad - 1u);
}

int zz_main(int argc, char **argv)
{
    uint8_t *buf;
    block *src;
    block *dst;
    uint32_t seed = 0x2545f491u;
    uint32_t i;
    uint32_t hash;
    char line[64];
    char *end;

    (void)argc;
    (void)argv;

    /* FNV-1a known answers: "a" and "foobar". */
    if (fnv1a32((const uint8_t *)"a", 1u) != 0xe40c292cu ||
        fnv1a32((const uint8_t *)"foobar", 6u) != 0xbf9cf968u) {
        fail();
        return 1;
    }
#ifdef ZZAPP_FREESTANDING
    /* Without ZZ_ALLOC_HOST_VISIBLE, zz_alloc has no heap to draw from. */
    if (zz_alloc(64u, 8u, 0u) != 0) {
        fail();
        return 1;
    }
#endif
    heap_init();
    buf = (uint8_t *)bump_alloc(BUF_BYTES, 64u);
    src = (block *)bump_alloc(sizeof(block), 8u);
    dst = (block *)bump_alloc(sizeof(block), 8u);
    if (!buf || !src || !dst) {
        fail();
        return 1;
    }
    for (i = 0; i < BUF_BYTES; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        buf[i] = (uint8_t)seed;
    }
    for (i = 0; i < sizeof src->bytes; ++i)
        src->bytes[i] = buf[i * 4099u];
    *dst = *src;
    for (i = 0; i < sizeof src->bytes; ++i) {
        if (dst->bytes[i] != src->bytes[i]) {
            fail();
            return 1;
        }
    }
    hash = fnv1a32(buf, BUF_BYTES);
    end = put_str(line, "freestanding ok: hash=");
    end = put_hex(end, hash);
    end = put_str(end, " heap_kib=");
    end = put_dec(end, (uint32_t)((uint8_t *)dst + sizeof *dst - (uint8_t *)buf) / 1024u);
    *end++ = '\n';
    zz_log(line, (uint32_t)(end - line));
    return 0;
}
