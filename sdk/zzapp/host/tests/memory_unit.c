/*
 * Unit check for host-visible allocation alignment in the host runner.
 * Includes memory.c directly so the slot table is observable.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/memory.c"

#include <assert.h>

static void test_visible_alignment(void)
{
    zzapp_api api;
    void *p;
    uint32_t offset;

    memset(&api, 0, sizeof api);
    host_memory_init();
    host_memory_bind(&api);
    p = api.alloc(4096u, 4096u, ZZ_ALLOC_HOST_VISIBLE);
    assert(p != NULL);
    /* malloc does not honour 4096; the returned pointer must. */
    assert(((uintptr_t)p & 4095u) == 0u);
    assert(api.host_address(p, &offset) == ZZ_OK);
    assert((offset & 4095u) == 0u);
    api.free(p);
    host_memory_shutdown();
}

static void test_arena_extent_checked_full_width(void)
{
    zzapp_api api;
    zzapp_mem_info mi;
    void *a;
    void *huge;
    void *b;

    memset(&api, 0, sizeof api);
    host_memory_init();
    host_memory_bind(&api);
    a = api.alloc(8u, 8u, 0u);
    assert(a != NULL);
    /* The 64-bit end narrowed to uint32_t used to wrap back below the
     * bump: this "succeeded" and rewound arena_used to overlap. */
    huge = api.alloc(0xFFFFFFFFu, 8u, 0u);
    assert(huge == NULL);
    b = api.alloc(8u, 8u, 0u);
    assert(b != NULL);
    assert(b != a);
    assert((char *)b >= (char *)a + 8);
    assert(api.mem_info(&mi) == ZZ_OK);
    assert(mi.arena_used == 16u);
    host_memory_shutdown();
}

int main(void)
{
    test_visible_alignment();
    test_arena_extent_checked_full_width();
    return 0;
}
