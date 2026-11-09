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

int zzapp_mem_init(zzapp_mem_t *memory, card_pool_t *pool)
{
    if (!memory || !pool || zzapp_mem_boot_check(ZZAPP_MEM_SYSTEM_RESERVE_BYTES))
        return -1;
    memset(memory, 0, sizeof(*memory));
    memory->lock = (sdk_smp_lock_t)SDK_SMP_LOCK_INIT;
    memory->pool = pool;
    memory->low_armed = 1;
    return 0;
}

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
        memory->extents[memory->extent_count].base = base;
        memory->extents[memory->extent_count].used_frames = 0;
        memory->extent_count++;
    }
    return 0;
}

int zzapp_mem_alloc(zzapp_mem_t *memory, zzapp_mem_proc_t *proc,
                    uint32_t frame_count, uint32_t *frames)
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
    for (uint32_t extent = 0, out = 0;
         extent < memory->extent_count && out < frame_count; extent++) {
        zzapp_mem_extent_t *entry = &memory->extents[extent];

        for (uint32_t frame = 0;
             frame < ZZAPP_MEM_FRAMES_PER_EXTENT && out < frame_count;
             frame++) {
            if (!(entry->used_frames & (1u << frame))) {
                uint32_t address = entry->base + frame * ZZAPP_MEM_FRAME_BYTES;

                entry->used_frames |= 1u << frame;
                proc->frames[proc->frame_count++] = address;
                frames[out++] = address;
                memset((void *)(uintptr_t)address, 0, ZZAPP_MEM_FRAME_BYTES);
            }
        }
    }
    proc->committed += bytes;
    memory->committed += bytes;
    low_event = update_water_locked(memory);
    sdk_smp_lock_release(&memory->pool->lock);
    sdk_smp_lock_release(&memory->lock);
    if (low_event && memory->low_callback)
        memory->low_callback(memory->low_context);
    return 0;
}

static int remove_proc_frame(zzapp_mem_proc_t *proc, uint32_t frame)
{
    for (uint32_t i = 0; i < proc->frame_count; i++) {
        if (proc->frames[i] == frame) {
            memmove(&proc->frames[i], &proc->frames[i + 1],
                    (proc->frame_count - i - 1u) * sizeof(proc->frames[0]));
            proc->frame_count--;
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

    if (remove_proc_frame(proc, frame))
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
            memory->extent_count--;
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
    if (!result)
        update_water_locked(memory);
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
