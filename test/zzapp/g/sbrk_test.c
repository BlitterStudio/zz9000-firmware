/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The app's newlib _sbrk against a runtime with and without heap_grow. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zzapp.h"

void *zzapp_sbrk(int increment);

#define MB(n) ((uint32_t)(n) << 20)

static int checks;
static int failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

/* Stands in for the app's private heap window: 1 MiB committed at start,
 * room to grow to 8 MiB. */
static uint8_t window[MB(8)] __attribute__((aligned(16)));
static uint32_t committed = MB(1);
static uint32_t grow_calls;
static uint32_t shrink_calls;

static void fake_heap_range(void **base, uint32_t *bytes)
{
    *base = window;
    *bytes = committed;
}

static int fake_heap_grow(uint32_t bytes, void **old_break)
{
    if (bytes % MB(1) || bytes > sizeof(window) - committed)
        return ZZ_ENOMEM;
    grow_calls++;
    *old_break = window + committed;
    committed += bytes;
    return ZZ_OK;
}

static int fake_heap_shrink(uint32_t bytes, void **new_break)
{
    if (bytes % MB(1) || bytes > committed)
        return ZZ_EINVAL;
    shrink_calls++;
    committed -= bytes;
    *new_break = window + committed;
    return ZZ_OK;
}

static zzapp_api dynamic_api;

/* One process image per scenario: _sbrk keeps its state in app BSS. */
static void test_dynamic_heap_grows_contiguously(void)
{
    uint8_t *first;
    uint8_t *second;

    memset(&dynamic_api, 0, sizeof(dynamic_api));
    dynamic_api.size = sizeof(dynamic_api);
    dynamic_api.heap_range = fake_heap_range;
    dynamic_api.heap_grow = fake_heap_grow;
    dynamic_api.heap_shrink = fake_heap_shrink;
    zzapp_api_ptr = &dynamic_api;

    first = zzapp_sbrk((int)(MB(1) - 16u));
    CHECK(first == window);
    CHECK(grow_calls == 0u);
    /* Crosses the committed end: one grow, memory stays contiguous. */
    second = zzapp_sbrk(64);
    CHECK(second == window + MB(1) - 16u);
    CHECK(grow_calls == 1u);
    CHECK(committed == MB(2));
    /* A large request grows by whole MiB in one call. */
    CHECK(zzapp_sbrk((int)MB(3)) == window + MB(1) + 48u);
    CHECK(committed == MB(5));
    /* More than the window holds fails and leaves the break alone. */
    CHECK(zzapp_sbrk((int)MB(4)) == (void *)-1);
    CHECK(zzapp_sbrk(0) == window + MB(4) + 48u);
    /* Giving back 3 MiB returns whole MiB above the break, keeping one. */
    CHECK(zzapp_sbrk(-(int)MB(3)) == window + MB(4) + 48u);
    CHECK(shrink_calls == 1u);
    CHECK(committed == MB(3));
    CHECK(zzapp_sbrk(0) == window + MB(1) + 48u);
}

int main(void)
{
    test_dynamic_heap_grows_contiguously();
    printf("zzapp_sbrk: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
