/*
 * Unit checks for the ZZAPP_FREESTANDING build of libzzapp. zzapp.c is
 * compiled with the macro set, so zz_alloc and zz_free take their no-libc
 * paths, and freestanding.c is included under other names so its mem*
 * routines can be compared with a byte-wise reference on the host.
 *
 * SPDX-License-Identifier: MIT
 */
#define ZZAPP_FREESTANDING 1
#define memcpy zzfs_memcpy
#define memmove zzfs_memmove
#define memset zzfs_memset
#define memcmp zzfs_memcmp
#include "../../src/freestanding.c"
#undef memcpy
#undef memmove
#undef memset
#undef memcmp
#include "../../src/zzapp.c"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned host_frees;
static void *host_block;

static void *fake_alloc(uint32_t bytes, uint32_t align, uint32_t flags)
{
    (void)align;
    assert(flags == ZZ_ALLOC_HOST_VISIBLE);
    host_block = calloc(1u, bytes);
    return host_block;
}

static void fake_free(void *p)
{
    assert(p == host_block);
    free(p);
    host_block = 0;
    ++host_frees;
}

static void test_alloc_without_libc_heap(void)
{
    zzapp_api api;
    void *p;
    uint32_t i;

    memset(&api, 0, sizeof api);
    api.size = (uint32_t)sizeof api;
    api.alloc = fake_alloc;
    api.free = fake_free;
    zzapp_api_ptr = &api;

    /* No ZZ_ALLOC_HOST_VISIBLE: the app owns the heap, so there is nothing
     * to allocate from. */
    assert(zz_alloc(64u, 8u, 0u) == NULL);
    assert(zz_alloc(64u, 8u, ZZ_ALLOC_UNCACHED) == NULL);
    assert(zz_alloc(64u, 8u, 0x80u) == NULL);

    p = zz_alloc(64u, 8u, ZZ_ALLOC_HOST_VISIBLE);
    assert(p != NULL && p == host_block);
    zz_free(p);
    assert(host_frees == 1u && host_block == NULL);

    /* A pointer zz_alloc never returned is not freed and not dereferenced. */
    zz_free(&i);
    zz_free(NULL);
    assert(host_frees == 1u);
}

enum { BUF = 96 };

static void fill(uint8_t *p, size_t n, unsigned seed)
{
    size_t i;

    for (i = 0; i < n; ++i)
        p[i] = (uint8_t)(seed * 31u + i * 7u + (i >> 3));
}

static void test_mem_routines(void)
{
    uint32_t backing_a[BUF / 4 + 4], backing_b[BUF / 4 + 4];
    uint8_t *a = (uint8_t *)backing_a;
    uint8_t *b = (uint8_t *)backing_b;
    uint8_t ref[BUF + 16];
    size_t off_d, off_s, n;

    for (off_d = 0; off_d < 4; ++off_d) {
        for (off_s = 0; off_s < 4; ++off_s) {
            for (n = 0; n <= 40; ++n) {
                fill(a, sizeof backing_a, 1);
                fill(b, sizeof backing_b, 2);
                memcpy(ref, a, sizeof backing_a);
                memcpy(ref + off_d, b + off_s, n);
                assert(zzfs_memcpy(a + off_d, b + off_s, n) == a + off_d);
                assert(memcmp(a, ref, sizeof backing_a) == 0);

                /* Overlapping moves in both directions. */
                fill(a, sizeof backing_a, 3);
                memcpy(ref, a, sizeof backing_a);
                memmove(ref + off_d + 4, ref + off_s, n);
                assert(zzfs_memmove(a + off_d + 4, a + off_s, n) ==
                       a + off_d + 4);
                assert(memcmp(a, ref, sizeof backing_a) == 0);
                fill(a, sizeof backing_a, 4);
                memcpy(ref, a, sizeof backing_a);
                memmove(ref + off_d, ref + off_s + 4, n);
                assert(zzfs_memmove(a + off_d, a + off_s + 4, n) == a + off_d);
                assert(memcmp(a, ref, sizeof backing_a) == 0);

                fill(a, sizeof backing_a, 5);
                memcpy(ref, a, sizeof backing_a);
                memset(ref + off_d, 0xa5, n);
                assert(zzfs_memset(a + off_d, 0x1a5, n) == a + off_d);
                assert(memcmp(a, ref, sizeof backing_a) == 0);
            }
        }
    }

    fill(a, 32, 6);
    memcpy(b, a, 32);
    assert(zzfs_memcmp(a, b, 32) == 0);
    assert(zzfs_memcmp(a, b, 0) == 0);
    b[17] = (uint8_t)(a[17] + 1u);
    assert(zzfs_memcmp(a, b, 32) < 0);
    assert(zzfs_memcmp(b, a, 32) > 0);
    assert(zzfs_memcmp(a, b, 17) == 0);
    a[3] = 0xff;
    b[3] = 0x01;
    assert(zzfs_memcmp(a, b, 32) > 0);
}

int main(void)
{
    test_alloc_without_libc_heap();
    test_mem_routines();
    return 0;
}
