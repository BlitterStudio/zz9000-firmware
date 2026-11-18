/*
 * memmove must not depend on the weak memcpy: an app may legally replace
 * memcpy with a copy that runs backward. This program links freestanding.c
 * as its own object (names prefixed zzfs_ by the build) and defines a strong,
 * backward-copying zzfs_memcpy that overrides the weak one.
 *
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void *zzfs_memmove(void *dst, const void *src, size_t n);

static unsigned replacement_calls;

void *zzfs_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst + n;
    const uint8_t *s = (const uint8_t *)src + n;

    replacement_calls++;
    while (n--)
        *--d = *--s;
    return dst;
}

int main(void)
{
    uint8_t buf[64];
    uint8_t ref[64];
    size_t off;
    size_t n;

    for (off = 1; off <= 12; ++off) {
        for (n = 1; n <= 40; ++n) {
            size_t i;

            for (i = 0; i < sizeof buf; ++i)
                buf[i] = (uint8_t)(i * 5u + 1u);
            memcpy(ref, buf, sizeof buf);
            /* Overlapping, destination below source: forward is required. */
            memmove(ref, ref + off, n);
            assert(zzfs_memmove(buf, buf + off, n) == buf);
            assert(memcmp(buf, ref, sizeof buf) == 0);
            /* Destination above source. */
            for (i = 0; i < sizeof buf; ++i)
                buf[i] = (uint8_t)(i * 7u + 3u);
            memcpy(ref, buf, sizeof buf);
            memmove(ref + off, ref, n);
            assert(zzfs_memmove(buf + off, buf, n) == buf + off);
            assert(memcmp(buf, ref, sizeof buf) == 0);
        }
    }
    (void)replacement_calls;
    return 0;
}
