/*
 * Unit check for host-visible allocation alignment in the host runner.
 * Includes memory.c directly so the slot table is observable.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/memory.c"

#include <assert.h>

int main(void)
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
    return 0;
}
