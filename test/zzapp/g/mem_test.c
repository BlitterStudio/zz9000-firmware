/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

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

static jmp_buf interrupted_allocation;
static int interrupt_allocation;
static enum zzapp_mem_test_store interrupt_store;

void zzapp_mem_test_frame_store(zzapp_mem_t *test_memory,
                                zzapp_mem_proc_t *test_proc,
                                enum zzapp_mem_test_store store)
{
    (void)test_memory;
    (void)test_proc;
    if (interrupt_allocation && store == interrupt_store) {
        interrupt_allocation = 0;
        longjmp(interrupted_allocation, 1);
    }
}

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

static void test_interrupted_allocation_reconciles_each_store(void)
{
    volatile enum zzapp_mem_test_store store;

    for (store = ZZAPP_MEM_TEST_AFTER_PROC_POINTER;
         store <= ZZAPP_MEM_TEST_AFTER_MEMORY_CHARGE; store++) {
        uint32_t frame;
        uint32_t committed;
        uint32_t free_bytes;
        uint32_t pool_free;

        reset_memory();
        pool_free = card_pool_range_free_bytes(&card_pool, 0);
        CHECK(zzapp_mem_alloc(&memory, &proc_a, 1, &frame) == 0);
        committed = memory.committed;
        free_bytes = zzapp_mem_free_bytes(&memory);
        if (setjmp(interrupted_allocation) == 0) {
            interrupt_store = store;
            interrupt_allocation = 1;
            (void)zzapp_mem_alloc(&memory, &proc_b, 1, &frame);
            CHECK(0);
        }
        card_pool_reset_lock(&card_pool);
        zzapp_mem_reset_live_lock();
        zzapp_mem_reconcile_halted();
        (void)zzapp_mem_release_proc(&memory, &proc_b);
        CHECK(proc_b.committed == 0U);
        CHECK(memory.proc_count == 1U);
        CHECK(memory.committed == committed);
        CHECK(zzapp_mem_free_bytes(&memory) == free_bytes);
        CHECK(zzapp_mem_release_proc(&memory, &proc_a) == MB(1));
        CHECK(card_pool_range_free_bytes(&card_pool, 0) == pool_free);
    }
}

static void test_unpublished_extent_is_reclaimed(void)
{
    uint32_t frame;
    uint32_t pool_free;

    reset_memory();
    pool_free = card_pool_range_free_bytes(&card_pool, 0);
    if (setjmp(interrupted_allocation) == 0) {
        interrupt_store = ZZAPP_MEM_TEST_AFTER_POOL_ALLOCATION;
        interrupt_allocation = 1;
        (void)zzapp_mem_alloc(&memory, &proc_b, 1, &frame);
        CHECK(0);
    }
    card_pool_reset_lock(&card_pool);
    zzapp_mem_reset_live_lock();
    zzapp_mem_reconcile_halted();
    CHECK(memory.extent_count == 0U);
    CHECK(memory.committed == 0U);
    CHECK(card_pool_range_free_bytes(&card_pool, 0) == pool_free);
}

static void test_compacted_proc_registry_reconciles_once(void)
{
    uint32_t frame_a;
    uint32_t frame_b;
    uint32_t pool_free;

    reset_memory();
    pool_free = card_pool_range_free_bytes(&card_pool, 0);
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 1, &frame_a) == 0);
    CHECK(zzapp_mem_alloc(&memory, &proc_b, 1, &frame_b) == 0);
    if (setjmp(interrupted_allocation) == 0) {
        interrupt_store = ZZAPP_MEM_TEST_AFTER_PROC_COMPACT;
        interrupt_allocation = 1;
        (void)zzapp_mem_release_proc(&memory, &proc_a);
        CHECK(0);
    }
    card_pool_reset_lock(&card_pool);
    zzapp_mem_reset_live_lock();
    zzapp_mem_reconcile_halted();
    CHECK(memory.proc_count == 1U);
    CHECK(memory.committed == MB(1));
    CHECK(zzapp_mem_release_proc(&memory, &proc_b) == MB(1));
    CHECK(memory.committed == 0U);
    CHECK(card_pool_range_free_bytes(&card_pool, 0) == pool_free);
}

static void test_compacted_frame_records_reconcile_once(void)
{
    uint32_t frames[2];
    uint32_t pool_free;

    reset_memory();
    pool_free = card_pool_range_free_bytes(&card_pool, 0);
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 2, frames) == 0);
    if (setjmp(interrupted_allocation) == 0) {
        interrupt_store = ZZAPP_MEM_TEST_AFTER_FRAME_COMPACT;
        interrupt_allocation = 1;
        (void)zzapp_mem_release(&memory, &proc_a, frames[0]);
        CHECK(0);
    }
    card_pool_reset_lock(&card_pool);
    zzapp_mem_reset_live_lock();
    zzapp_mem_reconcile_halted();
    CHECK(proc_a.frame_count == 1U);
    CHECK(proc_a.frames[1] == 0U);
    CHECK(proc_a.committed == MB(1));
    CHECK(memory.committed == MB(1));
    CHECK(zzapp_mem_release_proc(&memory, &proc_a) == MB(1));
    CHECK(memory.committed == 0U);
    CHECK(card_pool_range_free_bytes(&card_pool, 0) == pool_free);
}

static void test_compacted_extent_registry_reconciles_once(void)
{
    uint32_t frames[ZZAPP_MEM_FRAMES_PER_EXTENT + 1U];
    uint32_t pool_free;
    uint32_t i;

    reset_memory();
    pool_free = card_pool_range_free_bytes(&card_pool, 0);
    CHECK(zzapp_mem_alloc(&memory, &proc_a,
                          ZZAPP_MEM_FRAMES_PER_EXTENT + 1U, frames) == 0);
    if (setjmp(interrupted_allocation) == 0) {
        interrupt_store = ZZAPP_MEM_TEST_AFTER_EXTENT_COMPACT;
        interrupt_allocation = 1;
        for (i = 0; i < ZZAPP_MEM_FRAMES_PER_EXTENT; i++)
            CHECK(zzapp_mem_release(&memory, &proc_a, frames[i]) == 0);
        CHECK(0);
    }
    card_pool_reset_lock(&card_pool);
    zzapp_mem_reset_live_lock();
    zzapp_mem_reconcile_halted();
    CHECK(memory.extent_count == 1U);
    CHECK(proc_a.frame_count == 1U);
    CHECK(memory.committed == MB(1));
    CHECK(card_pool_range_free_bytes(&card_pool, 0) ==
          pool_free - ZZAPP_MEM_EXTENT_BYTES);
    CHECK(zzapp_mem_release_proc(&memory, &proc_a) == MB(1));
    CHECK(card_pool_range_free_bytes(&card_pool, 0) == pool_free);
}

static void test_last_frame_count_reconciles_commitment(void)
{
    uint32_t frame;

    reset_memory();
    zzapp_mem_proc_init(&proc_a, MB(1));
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 1, &frame) == 0);
    if (setjmp(interrupted_allocation) == 0) {
        interrupt_store = ZZAPP_MEM_TEST_AFTER_FRAME_COUNT;
        interrupt_allocation = 1;
        (void)zzapp_mem_release(&memory, &proc_a, frame);
        CHECK(0);
    }
    card_pool_reset_lock(&card_pool);
    zzapp_mem_reset_live_lock();
    zzapp_mem_reconcile_halted();
    CHECK(proc_a.frame_count == 0U);
    CHECK(proc_a.committed == 0U);
    CHECK(memory.committed == 0U);
    CHECK(memory.proc_count == 0U);
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 1, &frame) == 0);
    CHECK(zzapp_mem_release_proc(&memory, &proc_a) == MB(1));
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

static void test_orphaned_allocator_lock_does_not_deadlock_release(void)
{
    uint32_t frames[2];

    reset_memory();
    CHECK(zzapp_mem_alloc(&memory, &proc_a, 2, frames) == 0);
    memory.lock.owner = 1;
    memory.lock.depth = 1U;
    memory.lock.raw = 1U;
    zzapp_mem_reset_live_lock();
    CHECK(memory.lock.owner == -1);
    CHECK(memory.lock.depth == 0U);
    CHECK(memory.lock.raw == 0U);
    CHECK(zzapp_mem_release_proc(&memory, &proc_a) == MB(2));
    CHECK(proc_a.frame_count == 0U);
}

/* One proc per process record: the ledger tracks every record's proc, not a
 * fixed few. 64 procs share the extents (8 frames each), a halted core left
 * each with a duplicated frame record, and the extent bits and the committed
 * sum stale: the rebuild must come out exact. */
static zzapp_mem_proc_t many[64];

static void test_sixty_four_procs_reconcile_with_shared_extents(void)
{
    uint32_t frame;
    uint32_t i;
    uint32_t used = 0U;
    uint32_t pool_free;

    reset_memory();
    pool_free = card_pool_range_free_bytes(&card_pool, 0);
    for (i = 0U; i < 64U; i++) {
        zzapp_mem_proc_init(&many[i], MB(2));
        CHECK(zzapp_mem_alloc(&memory, &many[i], 1, &frame) == 0);
    }
    CHECK(memory.proc_count == 64U);
    CHECK(memory.committed == MB(64));
    CHECK(memory.extent_count == 8U);
    for (i = 0U; i < 64U; i++) {
        many[i].frames[1] = many[i].frames[0];
        many[i].frame_count = 2U;
        many[i].committed = MB(2);
    }
    memory.committed = MB(999);
    for (i = 0U; i < memory.extent_count; i++)
        memory.extents[i].used_frames = 0xffU;
    card_pool_reset_lock(&card_pool);
    zzapp_mem_reset_live_lock();
    zzapp_mem_reconcile_halted();
    CHECK(memory.proc_count == 64U);
    CHECK(memory.committed == MB(64));
    CHECK(memory.extent_count == 8U);
    for (i = 0U; i < memory.extent_count; i++)
        used += (uint32_t)__builtin_popcount(memory.extents[i].used_frames);
    CHECK(used == 64U);
    for (i = 0U; i < 64U; i++) {
        CHECK(many[i].frame_count == 1U);
        CHECK(many[i].committed == MB(1));
    }
    for (i = 0U; i < 64U; i++)
        CHECK(zzapp_mem_release_proc(&memory, &many[i]) == MB(1));
    CHECK(memory.proc_count == 0U);
    CHECK(memory.committed == 0U);
    CHECK(card_pool_range_free_bytes(&card_pool, 0) == pool_free);
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
    test_orphaned_allocator_lock_does_not_deadlock_release();
    test_frames_leave_legacy_arena_class_alone();
    test_interrupted_allocation_reconciles_each_store();
    test_unpublished_extent_is_reclaimed();
    test_compacted_proc_registry_reconciles_once();
    test_compacted_frame_records_reconcile_once();
    test_compacted_extent_registry_reconciles_once();
    test_last_frame_count_reconciles_commitment();
    test_sixty_four_procs_reconcile_with_shared_extents();
    printf("zzapp_mem: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
