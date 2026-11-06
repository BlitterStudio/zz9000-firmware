/*
 * Host stand-in for the card arena and the host-visible heap.
 *
 * Ordinary zz_alloc (no HOST_VISIBLE) is served by libzzapp's malloc, not by
 * this file. The table slot still implements a bump arena so a direct table
 * call has a span, and heap_range reports that span. HOST_VISIBLE allocations
 * are ordinary process memory under a 16 MiB quota. host_address returns a
 * synthetic board offset, not a Zorro address, and rejects interior pointers.
 *
 * SPDX-License-Identifier: MIT
 */
#include "host_internal.h"

#include <stdlib.h>
#include <string.h>

struct host_visible {
    void *ptr;
    uint32_t bytes;
    uint32_t offset;
};

static uint8_t *arena;
static uint32_t arena_used;
static struct host_visible visible[HOST_VISIBLE_SLOTS];
static uint32_t visible_used;
static uint32_t visible_next = 0x033f0000u;

static int pow2_align(uint32_t align)
{
    return align >= 8u && (align & (align - 1u)) == 0u;
}

static void *arena_alloc(uint32_t bytes, uint32_t align)
{
    uintptr_t p;
    uint32_t bump;

    if (!arena)
        return 0;
    p = (uintptr_t)(arena + arena_used);
    p = (p + align - 1u) & ~(uintptr_t)(align - 1u);
    if (p + bytes < p)
        return 0;
    bump = (uint32_t)(p + bytes - (uintptr_t)arena);
    if (bump > HOST_ARENA_BYTES)
        return 0;
    arena_used = bump;
    return (void *)p;
}

static void *visible_alloc(uint32_t bytes, uint32_t align)
{
    uint32_t i;
    uint32_t offset;
    void *ptr;

    if (bytes > HOST_VISIBLE_LIMIT - visible_used)
        return 0;
    offset = (visible_next + align - 1u) & ~(align - 1u);
    if (offset < visible_next)
        return 0;
    for (i = 0; i < HOST_VISIBLE_SLOTS; i++) {
        if (!visible[i].ptr)
            break;
    }
    if (i == HOST_VISIBLE_SLOTS)
        return 0;
    ptr = malloc(bytes);
    if (!ptr)
        return 0;
    memset(ptr, 0, bytes);
    visible[i].ptr = ptr;
    visible[i].bytes = bytes;
    visible[i].offset = offset;
    visible_used += bytes;
    visible_next = offset + bytes;
    return ptr;
}

static void *api_alloc(uint32_t bytes, uint32_t align, uint32_t flags)
{
    if (!bytes || !pow2_align(align))
        return 0;
    if ((flags & ~(ZZ_ALLOC_UNCACHED | ZZ_ALLOC_HOST_VISIBLE)) != 0u)
        return 0;
    if (flags & ZZ_ALLOC_HOST_VISIBLE)
        return visible_alloc(bytes, align);
    return arena_alloc(bytes, align);
}

static void api_free(void *p)
{
    uint32_t i;

    if (!p)
        return;
    for (i = 0; i < HOST_VISIBLE_SLOTS; i++) {
        if (visible[i].ptr == p) {
            visible_used -= visible[i].bytes;
            free(p);
            visible[i].ptr = 0;
            visible[i].bytes = 0;
            visible[i].offset = 0;
            return;
        }
    }
}

static void api_heap_range(void **base, uint32_t *bytes)
{
    if (base)
        *base = arena;
    if (bytes)
        *bytes = arena ? HOST_ARENA_BYTES : 0u;
}

static int api_host_address(const void *p, uint32_t *host_offset)
{
    uint32_t i;

    if (!p || !host_offset)
        return ZZ_EINVAL;
    for (i = 0; i < HOST_VISIBLE_SLOTS; i++) {
        if (visible[i].ptr == p) {
            *host_offset = visible[i].offset;
            return ZZ_OK;
        }
    }
    return ZZ_EINVAL;
}

static int api_mem_info(zzapp_mem_info *out)
{
    if (!out)
        return ZZ_EINVAL;
    memset(out, 0, sizeof *out);
    out->arena_size = HOST_ARENA_BYTES;
    out->arena_used = arena_used;
    out->arena_free = HOST_ARENA_BYTES - arena_used;
    out->arena_largest_block = out->arena_free;
    out->host_visible_used = visible_used;
    out->host_visible_limit = HOST_VISIBLE_LIMIT;
    return ZZ_OK;
}

void host_memory_init(void)
{
    arena = (uint8_t *)malloc(HOST_ARENA_BYTES);
    arena_used = 0;
    visible_used = 0;
    visible_next = 0x033f0000u;
    memset(visible, 0, sizeof visible);
    if (arena)
        memset(arena, 0, HOST_ARENA_BYTES);
}

void host_memory_shutdown(void)
{
    uint32_t i;

    for (i = 0; i < HOST_VISIBLE_SLOTS; i++) {
        free(visible[i].ptr);
        visible[i].ptr = 0;
    }
    free(arena);
    arena = 0;
    arena_used = 0;
    visible_used = 0;
}

void host_memory_bind(zzapp_api *api)
{
    api->alloc = api_alloc;
    api->free = api_free;
    api->heap_range = api_heap_range;
    api->host_address = api_host_address;
    api->mem_info = api_mem_info;
}
