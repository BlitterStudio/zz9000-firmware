/*
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "memorymap.h"
#include "zzapp_mem.h"

#define ZZAPP_MEM_POOL_OWNER CARD_POOL_OWNER(CARD_POOL_CLASS_APP_FRAMES, 0)

int zzapp_mem_boot_check(uint32_t reserve)
{
    uint32_t permanent = CARD_POOL_PERMANENT_END - CARD_POOL_PERMANENT_ADDRESS;

    if (reserve > permanent)
        return -1;
    return permanent - reserve <= ZZAPP_MEM_PRIVATE_WINDOW_BYTES ? 0 : -1;
}

static zzapp_mem_t *live_memory;

int zzapp_mem_init(zzapp_mem_t *memory, card_pool_t *pool)
{
    if (!memory || !pool || zzapp_mem_boot_check(ZZAPP_MEM_SYSTEM_RESERVE_BYTES))
        return -1;
    memset(memory, 0, sizeof(*memory));
    memory->lock = (sdk_smp_lock_t)SDK_SMP_LOCK_INIT;
    memory->pool = pool;
    memory->low_armed = 1;
    live_memory = memory;
    return 0;
}

void zzapp_mem_reset_lock(zzapp_mem_t *memory)
{
    if (memory)
        sdk_smp_lock_reset(&memory->lock);
}

void zzapp_mem_reset_live_lock(void)
{
    zzapp_mem_reset_lock(live_memory);
}

#ifdef ZZAPP_MEM_HOST_TEST
#define FRAME_STORE(memory, proc, store) \
    zzapp_mem_test_frame_store(memory, proc, store)
#else
#define FRAME_STORE(memory, proc, store) ((void)0)
#endif

void zzapp_mem_proc_init(zzapp_mem_proc_t *proc, uint32_t limit)
{
    memset(proc, 0, sizeof(*proc));
    proc->limit = limit;
}

void zzapp_mem_set_low_memory_callback(zzapp_mem_t *memory,
                                       zzapp_mem_low_callback_t callback,
                                       void *context)
{
    sdk_smp_lock_acquire(&memory->lock);
    memory->low_callback = callback;
    memory->low_context = context;
    sdk_smp_lock_release(&memory->lock);
}

static uint32_t extent_free_frames(const zzapp_mem_extent_t *extent)
{
    uint32_t count = 0;

    for (uint32_t frame = 0; frame < ZZAPP_MEM_FRAMES_PER_EXTENT; frame++) {
        if (!(extent->used_frames & (1u << frame)))
            count++;
    }
    return count;
}

/* The pool no longer counts an extent as free, but every uncommitted frame
 * in it remains available to this allocator. Add the whole extent back to
 * reconstruct the capacity against which commits are admitted. */
static uint32_t available_bytes_locked(const zzapp_mem_t *memory)
{
    uint32_t bytes = card_pool_available_bytes(memory->pool,
                                               ZZAPP_MEM_POOL_OWNER);

    return bytes + memory->extent_count * ZZAPP_MEM_EXTENT_BYTES;
}

static int update_water_locked(zzapp_mem_t *memory)
{
    uint32_t available = available_bytes_locked(memory);
    uint32_t free_bytes = 0;

    if (available > ZZAPP_MEM_SYSTEM_RESERVE_BYTES + memory->committed)
        free_bytes = available - ZZAPP_MEM_SYSTEM_RESERVE_BYTES -
                     memory->committed;
    if (memory->low_armed && free_bytes < ZZAPP_MEM_LOW_WATER_BYTES) {
        memory->low_armed = 0;
        return 1;
    }
    if (!memory->low_armed && free_bytes > ZZAPP_MEM_HIGH_WATER_BYTES)
        memory->low_armed = 1;
    return 0;
}

static int add_extents_locked(zzapp_mem_t *memory, uint32_t count)
{
    uint32_t first = memory->extent_count;

    if (count > ZZAPP_MEM_MAX_EXTENTS - first)
        return -1;
    while (count--) {
        uint32_t base = card_pool_alloc_aligned(memory->pool,
                                                ZZAPP_MEM_EXTENT_BYTES,
                                                ZZAPP_MEM_FRAME_BYTES,
                                                ZZAPP_MEM_POOL_OWNER);

        if (!base) {
            while (memory->extent_count > first) {
                zzapp_mem_extent_t *extent =
                    &memory->extents[--memory->extent_count];
                card_pool_free(memory->pool, extent->base,
                               ZZAPP_MEM_POOL_OWNER);
            }
            return -1;
        }
        FRAME_STORE(memory, NULL, ZZAPP_MEM_TEST_AFTER_POOL_ALLOCATION);
        memory->extents[memory->extent_count].base = base;
        memory->extents[memory->extent_count].used_frames = 0;
        __atomic_store_n(&memory->extent_count, memory->extent_count + 1U,
                         __ATOMIC_RELEASE);
    }
    return 0;
}

static int register_proc_locked(zzapp_mem_t *memory, zzapp_mem_proc_t *proc)
{
    uint32_t count = __atomic_load_n(&memory->proc_count, __ATOMIC_ACQUIRE);
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (memory->procs[i] == proc)
            return 0;
    }
    if (count == ZZAPP_MEM_MAX_PROCS)
        return -1;
    memory->procs[count] = proc;
    FRAME_STORE(memory, proc, ZZAPP_MEM_TEST_AFTER_PROC_POINTER);
    __atomic_store_n(&memory->proc_count, count + 1U, __ATOMIC_RELEASE);
    return 0;
}

static void unregister_proc_locked(zzapp_mem_t *memory, zzapp_mem_proc_t *proc)
{
    uint32_t count = __atomic_load_n(&memory->proc_count, __ATOMIC_ACQUIRE);
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (memory->procs[i] != proc)
            continue;
        memmove(&memory->procs[i], &memory->procs[i + 1],
                (count - i - 1U) * sizeof(memory->procs[0]));
        FRAME_STORE(memory, proc, ZZAPP_MEM_TEST_AFTER_PROC_COMPACT);
        __atomic_store_n(&memory->proc_count, count - 1U, __ATOMIC_RELEASE);
        memory->procs[count - 1U] = NULL;
        return;
    }
}

static int extent_is_published(const zzapp_mem_t *memory, uint32_t base)
{
    uint32_t extent;

    for (extent = 0; extent < memory->extent_count; extent++) {
        if (memory->extents[extent].base == base)
            return 1;
    }
    return 0;
}

static void free_unpublished_extents(zzapp_mem_t *memory)
{
    uint32_t block = 0U;

    while (block < memory->pool->block_count) {
        card_pool_block_t *entry = &memory->pool->blocks[block];

        if (entry->owner == ZZAPP_MEM_POOL_OWNER &&
            !(entry->flags & CARD_POOL_BLOCK_QUARANTINED) &&
            !extent_is_published(memory, entry->addr)) {
            card_pool_free(memory->pool, entry->addr, ZZAPP_MEM_POOL_OWNER);
            continue;
        }
        block++;
    }
}

static void dedupe_extents(zzapp_mem_t *memory)
{
    uint32_t count = memory->extent_count;
    uint32_t extent;
    uint32_t unique = 0U;

    for (extent = 0; extent < count; extent++) {
        uint32_t seen;

        for (seen = 0U; seen < unique; seen++) {
            if (memory->extents[seen].base == memory->extents[extent].base)
                break;
        }
        if (seen < unique)
            continue;
        memory->extents[unique++] = memory->extents[extent];
    }
    for (extent = unique; extent < count; extent++)
        memory->extents[extent] = (zzapp_mem_extent_t){0};
    memory->extent_count = unique;
}

static void rebuild_extent_bits(zzapp_mem_t *memory)
{
    uint32_t extent;
    uint32_t proc;
    uint32_t proc_count = __atomic_load_n(&memory->proc_count,
                                          __ATOMIC_ACQUIRE);
    uint32_t unique = 0U;

    dedupe_extents(memory);
    for (extent = 0; extent < memory->extent_count; extent++)
        memory->extents[extent].used_frames = 0U;
    memory->committed = 0U;
    for (proc = 0; proc < proc_count; proc++) {
        zzapp_mem_proc_t *entry = memory->procs[proc];
        uint32_t seen;
        uint32_t frame;
        uint32_t unique_frames = 0U;

        for (seen = 0U; seen < unique; seen++) {
            if (memory->procs[seen] == entry)
                break;
        }
        if (seen < unique)
            continue;
        for (frame = 0; frame < entry->frame_count; frame++) {
            uint32_t frame_seen;
            uint32_t address = entry->frames[frame];

            for (frame_seen = 0U; frame_seen < unique_frames; frame_seen++) {
                if (entry->frames[frame_seen] == address)
                    break;
            }
            if (frame_seen < unique_frames)
                continue;
            entry->frames[unique_frames++] = address;
        }
        for (frame = unique_frames; frame < entry->frame_count; frame++)
            entry->frames[frame] = 0U;
        entry->frame_count = unique_frames;
        entry->committed = unique_frames * ZZAPP_MEM_FRAME_BYTES;
        if (unique_frames == 0U)
            continue;
        memory->procs[unique++] = entry;
        memory->committed += entry->committed;
        for (frame = 0; frame < unique_frames; frame++) {
            uint32_t address = entry->frames[frame];

            for (extent = 0; extent < memory->extent_count; extent++) {
                zzapp_mem_extent_t *current = &memory->extents[extent];
                uint32_t offset;

                if (address < current->base)
                    continue;
                offset = address - current->base;
                if (offset < ZZAPP_MEM_EXTENT_BYTES &&
                    offset % ZZAPP_MEM_FRAME_BYTES == 0U) {
                    current->used_frames |=
                        1u << (offset / ZZAPP_MEM_FRAME_BYTES);
                    break;
                }
            }
        }
    }
    for (proc = unique; proc < proc_count; proc++)
        memory->procs[proc] = NULL;
    __atomic_store_n(&memory->proc_count, unique, __ATOMIC_RELEASE);
}

void zzapp_mem_reconcile_halted(void)
{
    zzapp_mem_t *memory = live_memory;
    uint32_t extent;

    if (!memory)
        return;
    sdk_smp_lock_acquire(&memory->lock);
    sdk_smp_lock_acquire(&memory->pool->lock);
    rebuild_extent_bits(memory);
    for (extent = 0; extent < memory->extent_count;) {
        zzapp_mem_extent_t *entry = &memory->extents[extent];

        if (entry->used_frames != 0U) {
            extent++;
            continue;
        }
        card_pool_free(memory->pool, entry->base, ZZAPP_MEM_POOL_OWNER);
        memmove(entry, entry + 1,
                (memory->extent_count - extent - 1u) * sizeof(*entry));
        memory->extent_count--;
    }
    free_unpublished_extents(memory);
    update_water_locked(memory);
    sdk_smp_lock_release(&memory->pool->lock);
    sdk_smp_lock_release(&memory->lock);
}

static int alloc_frames(zzapp_mem_t *memory, zzapp_mem_proc_t *proc,
                        uint32_t frame_count, uint32_t *frames,
                        int zero_frames)
{
    uint32_t bytes;
    uint32_t free_frames = 0;
    int low_event;

    if (!memory || !proc || !frames || !frame_count ||
        frame_count > ZZAPP_MEM_MAX_FRAMES - proc->frame_count)
        return -1;
    bytes = frame_count * ZZAPP_MEM_FRAME_BYTES;
    if (bytes > proc->limit - proc->committed)
        return -1;

    sdk_smp_lock_acquire(&memory->lock);
    sdk_smp_lock_acquire(&memory->pool->lock);
    {
        uint32_t available = available_bytes_locked(memory);

        if (available <= ZZAPP_MEM_SYSTEM_RESERVE_BYTES ||
            memory->committed > available - ZZAPP_MEM_SYSTEM_RESERVE_BYTES ||
            bytes > available - ZZAPP_MEM_SYSTEM_RESERVE_BYTES -
                    memory->committed) {
            sdk_smp_lock_release(&memory->pool->lock);
            sdk_smp_lock_release(&memory->lock);
            return -1;
        }
    }
    for (uint32_t extent = 0; extent < memory->extent_count; extent++)
        free_frames += extent_free_frames(&memory->extents[extent]);
    if (free_frames < frame_count &&
        add_extents_locked(memory, (frame_count - free_frames +
                                    ZZAPP_MEM_FRAMES_PER_EXTENT - 1u) /
                                   ZZAPP_MEM_FRAMES_PER_EXTENT)) {
        sdk_smp_lock_release(&memory->pool->lock);
        sdk_smp_lock_release(&memory->lock);
        return -1;
    }
    if (register_proc_locked(memory, proc)) {
        sdk_smp_lock_release(&memory->pool->lock);
        sdk_smp_lock_release(&memory->lock);
        return -1;
    }
    for (uint32_t extent = 0, out = 0;
         extent < memory->extent_count && out < frame_count; extent++) {
        zzapp_mem_extent_t *entry = &memory->extents[extent];

        for (uint32_t frame = 0;
             frame < ZZAPP_MEM_FRAMES_PER_EXTENT && out < frame_count;
             frame++) {
            if (!(entry->used_frames & (1u << frame))) {
                uint32_t address = entry->base + frame * ZZAPP_MEM_FRAME_BYTES;

                uint32_t count = proc->frame_count;

                entry->used_frames |= 1u << frame;
                FRAME_STORE(memory, proc, ZZAPP_MEM_TEST_AFTER_EXTENT_BIT);
                proc->frames[count] = address;
                __atomic_store_n(&proc->frame_count, count + 1U,
                                 __ATOMIC_RELEASE);
                FRAME_STORE(memory, proc, ZZAPP_MEM_TEST_AFTER_FRAME_RECORD);
                proc->committed += ZZAPP_MEM_FRAME_BYTES;
                FRAME_STORE(memory, proc, ZZAPP_MEM_TEST_AFTER_PROC_CHARGE);
                memory->committed += ZZAPP_MEM_FRAME_BYTES;
                FRAME_STORE(memory, proc, ZZAPP_MEM_TEST_AFTER_MEMORY_CHARGE);
                frames[out++] = address;
                if (zero_frames)
                    memset((void *)(uintptr_t)address, 0,
                           ZZAPP_MEM_FRAME_BYTES);
            }
        }
    }
    low_event = update_water_locked(memory);
    sdk_smp_lock_release(&memory->pool->lock);
    sdk_smp_lock_release(&memory->lock);
    if (low_event && memory->low_callback)
        memory->low_callback(memory->low_context);
    return 0;
}

int zzapp_mem_alloc(zzapp_mem_t *memory, zzapp_mem_proc_t *proc,
                    uint32_t frame_count, uint32_t *frames)
{
    return alloc_frames(memory, proc, frame_count, frames, 1);
}

int zzapp_mem_alloc_unzeroed(zzapp_mem_t *memory, zzapp_mem_proc_t *proc,
                             uint32_t frame_count, uint32_t *frames)
{
    return alloc_frames(memory, proc, frame_count, frames, 0);
}

uint32_t zzapp_mem_zero_frames(const zzapp_mem_proc_t *proc,
                               uint32_t first_frame, uint32_t max_frames)
{
    uint32_t count;
    uint32_t frame;

    if (!proc || first_frame >= proc->frame_count || max_frames == 0U)
        return 0U;
    count = proc->frame_count - first_frame;
    if (count > max_frames)
        count = max_frames;
    for (frame = 0U; frame < count; frame++)
        memset((void *)(uintptr_t)proc->frames[first_frame + frame], 0,
               ZZAPP_MEM_FRAME_BYTES);
    return count;
}

static int remove_proc_frame(zzapp_mem_t *memory, zzapp_mem_proc_t *proc,
                             uint32_t frame)
{
#ifndef ZZAPP_MEM_HOST_TEST
    (void)memory;
#endif
    for (uint32_t i = 0; i < proc->frame_count; i++) {
        if (proc->frames[i] == frame) {
            memmove(&proc->frames[i], &proc->frames[i + 1],
                    (proc->frame_count - i - 1u) * sizeof(proc->frames[0]));
            FRAME_STORE(memory, proc, ZZAPP_MEM_TEST_AFTER_FRAME_COMPACT);
            /* Publish after compaction so recovery sees its replacement prefix. */
            __atomic_store_n(&proc->frame_count, proc->frame_count - 1U,
                             __ATOMIC_RELEASE);
            FRAME_STORE(memory, proc, ZZAPP_MEM_TEST_AFTER_FRAME_COUNT);
            return 0;
        }
    }
    return -1;
}

static int release_frame_locked(zzapp_mem_t *memory, zzapp_mem_proc_t *proc,
                                uint32_t frame)
{
    uint32_t extent_index;
    uint32_t frame_index;

    if (remove_proc_frame(memory, proc, frame))
        return -1;
    for (extent_index = 0; extent_index < memory->extent_count; extent_index++) {
        zzapp_mem_extent_t *extent = &memory->extents[extent_index];
        uint32_t offset;

        if (frame < extent->base)
            continue;
        offset = frame - extent->base;
        if (offset >= ZZAPP_MEM_EXTENT_BYTES ||
            offset % ZZAPP_MEM_FRAME_BYTES)
            continue;
        frame_index = offset / ZZAPP_MEM_FRAME_BYTES;
        if (!(extent->used_frames & (1u << frame_index)))
            return -1;
        extent->used_frames &= ~(1u << frame_index);
        proc->committed -= ZZAPP_MEM_FRAME_BYTES;
        memory->committed -= ZZAPP_MEM_FRAME_BYTES;
        if (!extent->used_frames) {
            card_pool_free(memory->pool, extent->base, ZZAPP_MEM_POOL_OWNER);
            memmove(extent, extent + 1,
                    (memory->extent_count - extent_index - 1u) *
                    sizeof(*extent));
            FRAME_STORE(memory, proc, ZZAPP_MEM_TEST_AFTER_EXTENT_COMPACT);
            /* Publish after compaction so recovery sees its replacement prefix. */
            __atomic_store_n(&memory->extent_count,
                             memory->extent_count - 1U, __ATOMIC_RELEASE);
        }
        return 0;
    }
    return -1;
}

int zzapp_mem_release(zzapp_mem_t *memory, zzapp_mem_proc_t *proc,
                      uint32_t frame)
{
    int result;

    if (!memory || !proc || !frame)
        return -1;
    sdk_smp_lock_acquire(&memory->lock);
    sdk_smp_lock_acquire(&memory->pool->lock);
    result = release_frame_locked(memory, proc, frame);
    if (!result) {
        if (proc->frame_count == 0U)
            unregister_proc_locked(memory, proc);
        update_water_locked(memory);
    }
    sdk_smp_lock_release(&memory->pool->lock);
    sdk_smp_lock_release(&memory->lock);
    return result;
}

uint32_t zzapp_mem_release_proc(zzapp_mem_t *memory, zzapp_mem_proc_t *proc)
{
    uint32_t released = 0;

    if (!memory || !proc)
        return 0;
    sdk_smp_lock_acquire(&memory->lock);
    sdk_smp_lock_acquire(&memory->pool->lock);
    while (proc->frame_count) {
        uint32_t frame = proc->frames[0];

        if (release_frame_locked(memory, proc, frame))
            break;
        released += ZZAPP_MEM_FRAME_BYTES;
    }
    if (proc->frame_count == 0U)
        unregister_proc_locked(memory, proc);
    update_water_locked(memory);
    sdk_smp_lock_release(&memory->pool->lock);
    sdk_smp_lock_release(&memory->lock);
    return released;
}

uint32_t zzapp_mem_free_bytes(zzapp_mem_t *memory)
{
    uint32_t available;
    uint32_t free_bytes = 0;

    sdk_smp_lock_acquire(&memory->lock);
    sdk_smp_lock_acquire(&memory->pool->lock);
    available = available_bytes_locked(memory);
    if (available > ZZAPP_MEM_SYSTEM_RESERVE_BYTES + memory->committed)
        free_bytes = available - ZZAPP_MEM_SYSTEM_RESERVE_BYTES -
                     memory->committed;
    sdk_smp_lock_release(&memory->pool->lock);
    sdk_smp_lock_release(&memory->lock);
    return free_bytes;
}
