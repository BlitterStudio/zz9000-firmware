/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "card_pool.h"
#include "host_card_pool.h"
#include "zzapp_mem.h"

#define MB(n) ((uint32_t)(n) << 20)

static int failures;
static int checks;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0u; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1u; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1u;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0u; }

static zzapp_mem_t memory;
static zzapp_mem_proc_t proc_a;
static zzapp_mem_proc_t proc_b;

static void reset_memory(void)
{
    host_card_pool_reset(MB(256));
    CHECK(zzapp_mem_init(&memory, &card_pool) == 0);
    zzapp_mem_proc_init(&proc_a, MB(128));
    zzapp_mem_proc_init(&proc_b, MB(128));
}

static void test_frames_and_empty_extent_return(void)
{
    uint32_t frames[3];
    uint32_t extra[2];
    uint32_t start;

    reset_memory();
    start = card_pool_range_free_bytes(&card_pool, 0);
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 3, frames) == 0);
    CHECK(proc_a.committed == MB(3));
    CHECK(memory.committed == MB(3));
    CHECK(zzapp_mem_release(&memory, &proc_a, frames[1]) == 0);
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 2, extra) == 0);
    CHECK(proc_a.committed == MB(4));
    CHECK(memory.committed == MB(4));
    CHECK(memory.extent_count == 1);
    CHECK(zzapp_mem_release_proc(&memory, &proc_a) == MB(4));
    CHECK(card_pool_range_free_bytes(&card_pool, 0) == start);
}

static void test_admission_preserves_ledgers(void)
{
    uint32_t frames[128];

    reset_memory();
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 128, frames) == 0);
    CHECK(zzapp_mem_alloc(&memory, &proc_b, 1, frames) != 0);
    CHECK(proc_b.committed == 0);
    CHECK(memory.committed == MB(128));
}

static void test_proc_limit(void)
{
    uint32_t frames[3];

    reset_memory();
    zzapp_mem_proc_init(&proc_a, MB(2));
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 3, frames) != 0);
    CHECK(proc_a.committed == 0);
    CHECK(memory.committed == 0);
}

static void test_alternating_admission(void)
{
    uint32_t frame;
    uint32_t allocations = 0;

    reset_memory();
    while (zzapp_mem_alloc(&memory, allocations & 1u ? &proc_a : &proc_b,
                           1, &frame) == 0)
        allocations++;
    CHECK(allocations == 128);
    CHECK(memory.committed == MB(128));
    CHECK(proc_a.committed + proc_b.committed == MB(128));
}

static void test_boot_reserve_bound(void)
{
    CHECK(zzapp_mem_boot_check(ZZAPP_MEM_SYSTEM_RESERVE_BYTES) == 0);
    CHECK(zzapp_mem_boot_check(MB(77)) != 0);
}

static void test_frames_are_zeroed(void)
{
    uint32_t old_extent;
    uint32_t frame;

    reset_memory();
    old_extent = card_pool_alloc_aligned(&card_pool, ZZAPP_MEM_EXTENT_BYTES,
                                         ZZAPP_MEM_FRAME_BYTES,
                                         CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA, 1));
    CHECK(old_extent != 0);
    memset((void *)(uintptr_t)old_extent, 0xa5, ZZAPP_MEM_EXTENT_BYTES);
    CHECK(card_pool_free(&card_pool, old_extent,
                         CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA, 1)) == 1);
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 1, &frame) == 0);
    CHECK(frame == old_extent);
    CHECK(*(const uint32_t *)(uintptr_t)frame == 0u);
}

static void test_proc_release_is_idempotent(void)
{
    uint32_t frames[2];

    reset_memory();
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 2, frames) == 0);
    CHECK(zzapp_mem_release_proc(&memory, &proc_a) == MB(2));
    CHECK(zzapp_mem_release_proc(&memory, &proc_a) == 0);
    CHECK(memory.committed == 0);
    CHECK(proc_a.committed == 0);
}

static uint32_t low_events;

static void low_memory(void *context)
{
    uint32_t *events = context;
    (*events)++;
}

static void test_low_water_rearms_above_high_water(void)
{
    uint32_t frames[128];
    uint32_t i;

    reset_memory();
    low_events = 0;
    zzapp_mem_set_low_memory_callback(&memory, low_memory, &low_events);
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 97, frames) == 0);
    CHECK(low_events == 1);
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 1, &frames[97]) == 0);
    CHECK(low_events == 1);
    for (i = 0; i < 35; i++)
        CHECK(zzapp_mem_release(&memory, &proc_a, frames[i]) == 0);
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 34, frames) == 0);
    CHECK(low_events == 2);
}

/* Frames past 96 MiB must leave the legacy 96 MiB app arena class intact:
 * the arena still fits once and a second arena is still refused. */
static void test_frames_leave_legacy_arena_class_alone(void)
{
    static uint32_t frames[104];
    const uint32_t legacy = CARD_POOL_OWNER(CARD_POOL_CLASS_APP, 1);

    reset_memory();
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 104, frames) == 0);
    CHECK(card_pool_alloc(&card_pool, MB(96), legacy) != 0);
    CHECK(card_pool_alloc(&card_pool, MB(1), legacy) == 0);
}

int main(void)
{
    test_frames_and_empty_extent_return();
    test_admission_preserves_ledgers();
    test_proc_limit();
    test_alternating_admission();
    test_boot_reserve_bound();
    test_frames_are_zeroed();
    test_proc_release_is_idempotent();
    test_low_water_rearms_above_high_water();
    test_frames_leave_legacy_arena_class_alone();
    printf("zzapp_mem: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
