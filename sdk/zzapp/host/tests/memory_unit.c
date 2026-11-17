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

static void test_dynamic_heap_through_table(void)
{
    zzapp_api api;
    zzapp_heap_info info;
    void *range = 0;
    void *old_break = 0;
    void *second = 0;
    void *new_break = 0;
    uint32_t range_bytes = 0;
    uint32_t committed;
    uint8_t *grown;
    uint8_t *reported_end;
    int (*saved_shrink)(uint32_t, void **);

    memset(&api, 0, sizeof api);
    api.size = (uint32_t)sizeof api;
    host_memory_init();
    host_memory_bind(&api);
    zzapp_api_ptr = &api;

    assert(api.heap_grow != NULL);
    assert(api.heap_shrink != NULL);
    assert(api.heap_info != NULL);

    api.heap_range(&range, &range_bytes);
    assert(range != NULL);
    assert(range_bytes == 0u);
    reported_end = range;
    assert(api.heap_info(&info) == ZZ_OK);
    assert(info.committed == range_bytes);
    assert(info.limit >= (2u << 20));
    assert(info.pool_free == info.limit);
    assert(api.heap_info(NULL) == ZZ_EINVAL);

    assert(api.heap_grow(0u, &old_break) == ZZ_EINVAL);
    assert(api.heap_grow(4096u, &old_break) == ZZ_EINVAL);
    assert(zz_heap_grow(0u, &old_break) == ZZ_EINVAL);

    assert(api.heap_grow(1u << 20, &old_break) == ZZ_OK);
    assert(old_break == reported_end);
    api.heap_range(&range, &range_bytes);
    assert(range_bytes == (1u << 20));
    grown = old_break;
    grown[0] = 0x5a;
    grown[(1u << 20) - 1u] = 0xa5;

    reported_end = (uint8_t *)range + range_bytes;
    assert(zz_heap_grow(1u << 20, &second) == ZZ_OK);
    assert(second == reported_end);
    assert(grown[0] == 0x5a);
    assert(grown[(1u << 20) - 1u] == 0xa5);

    assert(api.heap_info(&info) == ZZ_OK);
    assert(info.committed == (2u << 20));
    assert(info.pool_free == info.limit - info.committed);
    while (info.committed + (1u << 20) <= info.limit) {
        assert(api.heap_grow(1u << 20, NULL) == ZZ_OK);
        assert(api.heap_info(&info) == ZZ_OK);
    }
    committed = info.committed;
    assert(committed == info.limit);
    assert(info.pool_free == 0u);
    assert(api.heap_grow(1u << 20, &old_break) == ZZ_ENOMEM);
    assert(zz_heap_grow(1u << 20, &old_break) == ZZ_ENOMEM);
    assert(api.heap_info(&info) == ZZ_OK);
    assert(info.committed == committed);
    assert(grown[0] == 0x5a);

    assert(zz_heap_shrink(1u << 20, &new_break) == ZZ_OK);
    assert(new_break == (uint8_t *)range + committed - (1u << 20));
    api.heap_range(&range, &range_bytes);
    assert(range_bytes == committed - (1u << 20));
    reported_end = (uint8_t *)range + range_bytes;
    assert(api.heap_shrink(0u, &new_break) == ZZ_EINVAL);
    assert(api.heap_shrink(4096u, &new_break) == ZZ_EINVAL);
    assert(api.heap_info(&info) == ZZ_OK);
    assert(info.committed == range_bytes);
    assert(api.heap_shrink(info.committed + (1u << 20), NULL) == ZZ_EINVAL);
    assert(api.heap_info(&info) == ZZ_OK);
    assert(info.committed == range_bytes);
    assert(api.heap_grow(1u << 20, &old_break) == ZZ_OK);
    assert(old_break == reported_end);
    api.heap_range(&range, &range_bytes);
    assert(range_bytes == committed);

    api.size = (uint32_t)offsetof(zzapp_api, heap_grow);
    assert(zz_heap_grow(1u << 20, &old_break) == ZZ_ENOSYS);
    assert(zz_heap_shrink(1u << 20, &new_break) == ZZ_ENOSYS);
    api.size = (uint32_t)sizeof api;
    saved_shrink = api.heap_shrink;
    api.heap_shrink = 0;
    assert(zz_heap_shrink(1u << 20, &new_break) == ZZ_ENOSYS);
    api.heap_shrink = saved_shrink;
    assert(api.heap_info(&info) == ZZ_OK);
    assert(info.committed == committed);

    host_memory_shutdown();
}

int main(void)
{
    test_visible_alignment();
    test_table_alloc_is_host_visible_only();
    test_dynamic_heap_through_table();
    return 0;
}
