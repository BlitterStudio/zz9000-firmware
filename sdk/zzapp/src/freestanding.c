/*
 * The four routines GCC may call on its own, even for code that never
 * names them: struct copies, zero initialisers and loop idioms become calls
 * to memcpy, memmove, memset and memcmp. libgcc does not provide them, so a
 * ZZAPP_FREESTANDING image needs a definition. These are weak: an app or a
 * faster library may define its own and this file steps aside. Nothing else
 * from <string.h> is provided; an app that calls strlen or strcmp supplies
 * them. Build this file with -fno-builtin and -fno-tree-loop-distribute-
 * patterns, or the loops below turn back into calls to themselves.
 *
 * Whole words move only when every address is 4-byte aligned, so no access
 * is unaligned and none depends on the mapping's memory type.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>

#define ZZAPP_WEAK __attribute__((weak))

/* Shared by memcpy and memmove. memmove must not call the weak memcpy: an
 * app may replace that with a copy that runs backward. */
static void copy_forward(uint8_t *d, const uint8_t *s, size_t n)
{
    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0u) {
        while (n >= 4u) {
            *(uint32_t *)(void *)d = *(const uint32_t *)(const void *)s;
            d += 4;
            s += 4;
            n -= 4u;
        }
    }
    while (n--)
        *d++ = *s++;
}

ZZAPP_WEAK void *memcpy(void *dst, const void *src, size_t n)
{
    copy_forward((uint8_t *)dst, (const uint8_t *)src, n);
    return dst;
}

ZZAPP_WEAK void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if ((uintptr_t)d <= (uintptr_t)s || (uintptr_t)d >= (uintptr_t)s + n) {
        copy_forward(d, s, n);
        return dst;
    }
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
    return dst;
}

ZZAPP_WEAK void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint8_t byte = (uint8_t)c;

    if (((uintptr_t)d & 3u) == 0u) {
        uint32_t word = byte * 0x01010101u;

        while (n >= 4u) {
            *(uint32_t *)(void *)d = word;
            d += 4;
            n -= 4u;
        }
    }
    while (n--)
        *d++ = byte;
    return dst;
}

ZZAPP_WEAK int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;

    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        ++x;
        ++y;
    }
    return 0;
}
