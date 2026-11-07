/*
 * Unit checks for the host runner's table allocator.
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

static void test_table_alloc_is_host_visible_only(void)
{
    zzapp_api api;
    zzapp_mem_info mi;

    memset(&api, 0, sizeof api);
    host_memory_init();
    host_memory_bind(&api);
    /* The arena is the app's malloc heap; as on the card the table must
     * not hand out a second set of arena blocks. */
    assert(api.alloc(8u, 8u, 0u) == NULL);
    assert(api.alloc(8u, 8u, ZZ_ALLOC_UNCACHED) == NULL);
    assert(api.mem_info(&mi) == ZZ_OK);
    assert(mi.arena_used == 0u);
    host_memory_shutdown();
}

int main(void)
{
    test_visible_alignment();
    test_table_alloc_is_host_visible_only();
    return 0;
}
