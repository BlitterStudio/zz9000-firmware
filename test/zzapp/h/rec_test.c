/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Process records, handle table, record arena and the APP_ARENA card-pool
 * class (slice 5b): sizing, carve, admission and refusal, handle lifecycle,
 * stale lookups, unadmit, retire halts and reconcile.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "card_pool.h"
#include "host_card_pool.h"
#include "memorymap.h"
#include "xil_cache.h"
#include "zzapp_rec.h"

struct xil_cache_mock_state g_xil_cache_mock;
int smp_cpu_id(void) { return 0; }
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1U;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }

#define DDR_WB 0x15de6U
#define HEAP_78M (78U << 20)

static int checks;
static int failures;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

static uint32_t bsp[4096];

static void load_bsp(void)
{
    uint32_t i;

    for (i = 0U; i < 0x400U; i++)
        bsp[i] = (i << 20) | DDR_WB;
}

/* A host arena of `apps` records out of a fresh host card pool, then the
 * address-space template. Returns the arena base. */
static uint32_t setup_arena(uint32_t apps)
{
    zzapp_rec_layout_t lay;
    uint32_t addr;

    host_card_pool_reset(128U << 20);
    zzapp_rec_layout_for(apps, HEAP_78M, &lay);
    addr = card_pool_alloc_aligned(&card_pool, lay.bytes, ZZAPP_REC_ARENA_ALIGN,
        CARD_POOL_OWNER(CARD_POOL_CLASS_APP_ARENA, 0U));
    CHECK(addr != 0U);
    CHECK(zzapp_rec_arena_init((void *)(uintptr_t)addr, &lay) == 0);
    load_bsp();
    zzapp_as_init(bsp);
    return addr;
}

static zzapp_rec_t *admit(uint32_t window, uint32_t *handle)
{
    zzapp_rec_t *rec = NULL;

    if (zzapp_rec_admit(window, handle, &rec) != 0)
        return NULL;
    return rec;
}

/* ---- Sizing ------------------------------------------------------------ */

static uint32_t up(uint32_t v, uint32_t step) { return (v + step - 1U) / step * step; }

static void test_sizing(void)
{
    zzapp_rec_layout_t lay;
    const uint32_t permanent = CARD_POOL_PERMANENT_END - CARD_POOL_PERMANENT_ADDRESS;
    uint32_t apps;
    uint32_t l2;
    uint32_t kib;

    /* This board: 590 MiB permanent, 128 MiB commit reserve, 2 MiB per app. */
    CHECK(permanent == (590U << 20));
    apps = zzapp_rec_apps_for(permanent, 128U << 20);
    CHECK(apps == 231U);
    zzapp_rec_layout_for(apps, HEAP_78M, &lay);
    CHECK(lay.apps == 231U);
    CHECK(lay.l2_template == 16U);
    /* 78 MiB of host heap: 2 tables a MiB, rounded up to 32 -> 160. */
    CHECK(lay.l2_shared == 160U);
    CHECK(lay.l2_total == 16U + 231U * 16U + 160U);
    CHECK(lay.l2_total == 3872U);
    l2 = lay.l2_total;
    kib = 16U + 231U * 16U + (l2 * 1030U + 1023U) / 1024U;
    CHECK(lay.as_bytes == 16384U * 232U + l2 * 1030U);
    CHECK(lay.tables_off == up(lay.as_bytes, 4096U));
    CHECK(lay.stack_off == lay.tables_off + 231U * 8192U);
    CHECK(lay.record_off == lay.stack_off + 231U * 32768U);
    CHECK(lay.bytes == up(lay.record_off + 231U * 12288U, 1U << 20));
    CHECK(lay.bytes == (20U << 20));
    CHECK(lay.bytes % ZZAPP_REC_ARENA_ALIGN == 0U);
    /* About 3.4 percent of the permanent range. */
    CHECK(lay.bytes * 100U / permanent <= 4U);
    (void)kib;
    CHECK(lay.tables_off % 4096U == 0U && lay.stack_off % 4096U == 0U);
    CHECK(lay.record_off % 64U == 0U);

    /* The basis is exactly 2 MiB above the reserve. */
    CHECK(zzapp_rec_apps_for((128U << 20) + (2U << 20) * 50U, 128U << 20) == 50U);
    CHECK(zzapp_rec_apps_for((128U << 20) + (2U << 20) * 50U + 2097151U,
                             128U << 20) == 50U);
    CHECK(zzapp_rec_apps_for(256U << 20, 128U << 20) == 64U);

    /* Small memory floors at 8 records, whatever the reserve. */
    CHECK(zzapp_rec_apps_for(100U << 20, 128U << 20) == ZZAPP_REC_MIN_APPS);
    CHECK(zzapp_rec_apps_for(128U << 20, 128U << 20) == ZZAPP_REC_MIN_APPS);
    CHECK(zzapp_rec_apps_for(0U, 0U) == ZZAPP_REC_MIN_APPS);
    CHECK(zzapp_rec_apps_for(144U << 20, 128U << 20) == ZZAPP_REC_MIN_APPS);
    CHECK(zzapp_rec_apps_for(146U << 20, 128U << 20) == 9U);
    zzapp_rec_layout_for(3U, HEAP_78M, &lay);
    CHECK(lay.apps == ZZAPP_REC_MIN_APPS);

    /* Huge memory clamps at the 16-bit-index handle table bound. */
    CHECK(zzapp_rec_apps_for(0xffffffffU, 0U) == ZZAPP_REC_MAX_APPS);
    CHECK(zzapp_rec_apps_for(0xc0000000U, 128U << 20) == ZZAPP_REC_MAX_APPS);
    zzapp_rec_layout_for(1000000U, HEAP_78M, &lay);
    CHECK(lay.apps == 1024U);
    CHECK(lay.l2_total == 16U + 1024U * 16U + 160U);
    CHECK(lay.l2_total < 0xfffeU);
    CHECK(lay.bytes == up(up(16384U * 1025U + lay.l2_total * 1030U, 4096U) +
                          1024U * (8192U + 32768U + 12288U), 1U << 20));

    /* Shared tables follow the host heap: 2 a MiB, rounded up to 32. */
    zzapp_rec_layout_for(8U, 0U, &lay);
    CHECK(lay.l2_shared == 0U);
    zzapp_rec_layout_for(8U, 1U, &lay);
    CHECK(lay.l2_shared == 32U);
    zzapp_rec_layout_for(8U, 16U << 20, &lay);
    CHECK(lay.l2_shared == 32U);
    zzapp_rec_layout_for(8U, (16U << 20) + 1U, &lay);
    CHECK(lay.l2_shared == 64U);
    zzapp_rec_layout_for(8U, 0xffffffffU, &lay);
    CHECK(lay.l2_total < 0xfffeU);

    /* The record is a thread array element and fits its slot. */
    CHECK(offsetof(zzapp_rec_t, thread) == 0U);
    CHECK(sizeof(zzapp_rec_t) <= ZZAPP_REC_RECORD_BYTES);
    printf("record %zu of %u bytes, arena %u bytes for %u records\n",
           sizeof(zzapp_rec_t), ZZAPP_REC_RECORD_BYTES, 20U << 20, 231U);
}

/* ---- The APP_ARENA class ------------------------------------------------ */

static void test_arena_class(void)
{
    card_pool_t pool;
    uint32_t arena;
    uint32_t media;
    uint32_t app;
    uint32_t firmware;
    const uint32_t owner = CARD_POOL_OWNER(CARD_POOL_CLASS_APP_ARENA, 0U);

    /* The class never goes into a revocable range, even when the permanent
     * range is full, and it has no class limit. */
    card_pool_init(&pool);
    CHECK(card_pool_add_range(&pool, 0x0b000000U, 0x0c000000U, 0) == 0);
    CHECK(card_pool_add_range(&pool, 0x2fe00000U, 0x3fe00000U, 1) == 1);
    card_pool_open_range(&pool, 0U);
    card_pool_open_range(&pool, 1U);
    CHECK(pool.class_limit[CARD_POOL_CLASS_APP_ARENA] == UINT32_MAX);
    arena = card_pool_alloc_aligned(&pool, 4U << 20, 1U << 20, owner);
    CHECK(arena >= 0x0b000000U && arena < 0x0c000000U);
    CHECK((arena & ((1U << 20) - 1U)) == 0U);
    app = card_pool_alloc(&pool, 64U << 10,
                          CARD_POOL_OWNER(CARD_POOL_CLASS_APP, 1U));
    CHECK(app != 0U);
    firmware = card_pool_alloc(&pool, (12U << 20) - (64U << 10),
                               CARD_POOL_OWNER(CARD_POOL_CLASS_FIRMWARE, 1U));
    CHECK(firmware != 0U);
    /* The permanent range is full. The arena class and FIRMWARE stay out of
     * the revocable tail. */
    CHECK(card_pool_alloc_aligned(&pool, 1U << 20, 1U << 20, owner) == 0U);
    CHECK(card_pool_alloc(&pool, 64U << 10,
            CARD_POOL_OWNER(CARD_POOL_CLASS_FIRMWARE, 2U)) == 0U);
    /* Media memory does go to the revocable tail. */
    media = card_pool_alloc(&pool, 16U << 20,
                            CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA, 1U));
    CHECK(media >= 0x2fe00000U);
    /* An Amiga reset releases everything but FIRMWARE and APP_ARENA. */
    card_pool_finish_amiga_reset(&pool, 0, 0);
    CHECK(card_pool_block_owned(&pool, arena, 4U << 20, owner));
    CHECK(card_pool_block_owned(&pool, firmware, (12U << 20) - (64U << 10),
            CARD_POOL_OWNER(CARD_POOL_CLASS_FIRMWARE, 1U)));
    CHECK(!card_pool_block_owned(&pool, app, 64U << 10,
            CARD_POOL_OWNER(CARD_POOL_CLASS_APP, 1U)));
    CHECK(pool.class_used[CARD_POOL_CLASS_APP_ARENA] == (4U << 20));
    CHECK(pool.class_used[CARD_POOL_CLASS_APP] == 0U);
    CHECK(pool.class_used[CARD_POOL_CLASS_MEDIA] == 0U);
}

static void test_carve(void)
{
    const uint32_t owner = CARD_POOL_OWNER(CARD_POOL_CLASS_APP_ARENA, 0U);
    zzapp_rec_layout_t lay;
    const zzapp_rec_layout_t *got;
    uint32_t base;
    uint32_t apps;

    /* A roomy pool gives the full size. */
    host_card_pool_reset(256U << 20);
    CHECK(zzapp_rec_arena_carve(&card_pool, 256U << 20, 128U << 20,
                                HEAP_78M) == 0);
    got = zzapp_rec_layout();
    CHECK(got != NULL && got->apps == 64U);
    base = zzapp_rec_arena_base();
    CHECK(base != 0U && (base & ((1U << 20) - 1U)) == 0U);
    CHECK(card_pool_block_owned(&card_pool, base, got->bytes, owner));
    CHECK(card_pool.class_used[CARD_POOL_CLASS_APP_ARENA] == got->bytes);
    CHECK(zzapp_rec_capacity() == 64U);
    /* One block, whole, at the lowest address. */
    CHECK(card_pool.block_count == 1U);

    /* A pool too small for the wish gets half the records, then half again,
     * down to the floor. */
    host_card_pool_reset(6U << 20);
    {
        /* The arena wants a 1 MiB aligned start; what is left of the pool
         * above the first such address is what can hold it. */
        uint32_t first = up(card_pool.ranges[0].base, 1U << 20);
        uint32_t room = card_pool.ranges[0].end - first;

        apps = 1024U;
        for (;;) {
            zzapp_rec_layout_for(apps, HEAP_78M, &lay);
            if (lay.bytes <= room || apps == ZZAPP_REC_MIN_APPS)
                break;
            apps /= 2U;
        }
    }
    CHECK(zzapp_rec_arena_carve(&card_pool, 0xffffffffU, 0U, HEAP_78M) == 0);
    CHECK(zzapp_rec_capacity() == apps);
    CHECK(card_pool.class_used[CARD_POOL_CLASS_APP_ARENA] == lay.bytes);

    /* Nothing fits: no arena, no records, nothing left allocated. */
    host_card_pool_reset(512U << 10);
    CHECK(zzapp_rec_arena_carve(&card_pool, 0xffffffffU, 0U, HEAP_78M) == -1);
    CHECK(card_pool.class_used[CARD_POOL_CLASS_APP_ARENA] == 0U);
}

static void test_arena_init_validation(void)
{
    zzapp_rec_layout_t lay;
    uint32_t addr;
    zzapp_rec_layout_t bad;

    host_card_pool_reset(64U << 20);
    zzapp_rec_layout_for(8U, HEAP_78M, &lay);
    addr = card_pool_alloc_aligned(&card_pool, lay.bytes, ZZAPP_REC_ARENA_ALIGN,
        CARD_POOL_OWNER(CARD_POOL_CLASS_APP_ARENA, 0U));
    CHECK(addr != 0U);
    CHECK(zzapp_rec_arena_init(NULL, &lay) == -1);
    CHECK(zzapp_rec_arena_init((void *)(uintptr_t)addr, NULL) == -1);
    CHECK(zzapp_rec_arena_init((void *)(uintptr_t)(addr + 4096U), &lay) == -1);
    bad = lay;
    bad.apps = 7U;
    CHECK(zzapp_rec_arena_init((void *)(uintptr_t)addr, &bad) == -1);
    bad = lay;
    bad.bytes = bad.record_off;
    CHECK(zzapp_rec_arena_init((void *)(uintptr_t)addr, &bad) == -1);
    bad = lay;
    bad.stack_off = bad.tables_off;
    CHECK(zzapp_rec_arena_init((void *)(uintptr_t)addr, &bad) == -1);
    /* A failed init leaves no usable table behind. */
    CHECK(zzapp_rec_capacity() == 0U && zzapp_rec_by_pid(0U) == NULL);
    CHECK(zzapp_rec_admit(0U, NULL, NULL) == AAR_REASON_MEMORY);
    CHECK(zzapp_rec_arena_init((void *)(uintptr_t)addr, &lay) == 0);
    CHECK(zzapp_rec_capacity() == 8U);
}

/* A dirty arena comes up with zeroed records and the per-index constants. */
static void test_boot_clears_records(void)
{
    zzapp_rec_layout_t lay;
    uint32_t addr;
    uint32_t pid;

    host_card_pool_reset(64U << 20);
    zzapp_rec_layout_for(8U, HEAP_78M, &lay);
    addr = card_pool_alloc_aligned(&card_pool, lay.bytes, ZZAPP_REC_ARENA_ALIGN,
        CARD_POOL_OWNER(CARD_POOL_CLASS_APP_ARENA, 0U));
    memset((void *)(uintptr_t)addr, 0xa5, lay.bytes);
    CHECK(zzapp_rec_arena_init((void *)(uintptr_t)addr, &lay) == 0);
    for (pid = 0U; pid < 8U; pid++) {
        const zzapp_rec_t *rec = zzapp_rec_by_pid(pid);
        const uint8_t *bytes = (const uint8_t *)rec;
        size_t i;
        int zero = 1;

        CHECK(rec != NULL);
        CHECK(rec->thread.state == ZZAPP_THREAD_STOPPED);
        CHECK(rec->hdr.pid == pid && rec->hdr.window == ZZAPP_REC_NO_WINDOW);
        CHECK(rec->hdr.handle == 0U);
        for (i = sizeof(rec->thread); i < sizeof(*rec); i++) {
            if (i >= offsetof(zzapp_rec_t, hdr) &&
                i < offsetof(zzapp_rec_t, hdr) + sizeof(rec->hdr))
                continue;
            if (bytes[i])
                zero = 0;
        }
        CHECK(zero);
        CHECK(zzapp_rec_state(pid) == ZZAPP_REC_FREE);
        CHECK(zzapp_rec_handle(pid) == 0U);
    }
}

/* ---- Layout of the per-index areas ------------------------------------- */

static void test_index_areas(void)
{
    uint32_t base = setup_arena(8U);
    const zzapp_rec_layout_t *lay = zzapp_rec_layout();
    uint32_t pid;

    CHECK(lay != NULL && zzapp_rec_arena_base() == base);
    CHECK(base != 0U);
    for (pid = 0U; pid < 8U; pid++) {
        const zzapp_rec_t *rec = zzapp_rec_by_pid(pid);
        uint32_t l1 = (uint32_t)(uintptr_t)zzapp_as_pool_l1(pid);

        CHECK(rec->hdr.l1 == l1 && l1 % 16384U == 0U);
        CHECK(zzapp_rec_stack_top(pid) - zzapp_rec_stack_base(pid) ==
              ZZAPP_REC_STACK_BYTES);
        CHECK(rec->hdr.kstack_base == zzapp_rec_stack_base(pid));
        CHECK(rec->hdr.kstack_top == zzapp_rec_stack_top(pid));
        CHECK((uintptr_t)zzapp_rec_tables(pid) % 4096U == 0U);
        CHECK(rec->hdr.tables == (uint32_t)(uintptr_t)zzapp_rec_tables(pid));
        CHECK((uintptr_t)rec % 64U == 0U);
        if (pid) {
            const zzapp_rec_t *prev = zzapp_rec_by_pid(pid - 1U);

            CHECK((const uint8_t *)rec - (const uint8_t *)prev ==
                  ZZAPP_REC_RECORD_BYTES);
            CHECK(zzapp_rec_stack_base(pid) - zzapp_rec_stack_base(pid - 1U) ==
                  ZZAPP_REC_STACK_BYTES);
            CHECK((uint8_t *)zzapp_rec_tables(pid) -
                  (uint8_t *)zzapp_rec_tables(pid - 1U) ==
                  ZZAPP_REC_TABLES_BYTES);
            CHECK(l1 - (uint32_t)(uintptr_t)zzapp_as_pool_l1(pid - 1U) ==
                  ZZAPP_REC_L1_BYTES);
        }
    }
    /* Areas do not overlap and stay inside the arena. */
    CHECK((uint32_t)(uintptr_t)zzapp_rec_tables(0U) >= base + lay->as_bytes);
    CHECK(zzapp_rec_stack_base(0U) >=
          (uint32_t)(uintptr_t)zzapp_rec_tables(7U) + ZZAPP_REC_TABLES_BYTES);
    CHECK((uint32_t)(uintptr_t)zzapp_rec_by_pid(0U) >= zzapp_rec_stack_top(7U));
    CHECK((uint32_t)(uintptr_t)zzapp_rec_by_pid(7U) + ZZAPP_REC_RECORD_BYTES <=
          base + lay->bytes);
    CHECK(zzapp_rec_by_pid(8U) == NULL && zzapp_rec_stack_base(8U) == 0U &&
          zzapp_rec_tables(8U) == NULL);
}

/* ---- Handle table ------------------------------------------------------- */

static void test_capacity_refusal(void)
{
    uint32_t handle[8];
    uint32_t extra = 0U;
    zzapp_rec_t *rec;
    zzapp_rec_table_snapshot_t before;
    zzapp_rec_table_snapshot_t after;
    zzapp_rec_stats_t st;
    uint32_t i;

    setup_arena(8U);
    for (i = 0U; i < 8U; i++) {
        rec = admit(i, &handle[i]);
        CHECK(rec != NULL);
        CHECK(rec && rec->hdr.pid == i && rec->hdr.handle == handle[i]);
        CHECK(handle[i] != 0U && aar_handle_index(handle[i]) == i);
        CHECK(aar_handle_generation(handle[i]) == 1U);
        CHECK(zzapp_rec_state(i) == ZZAPP_REC_CLAIMED);
    }
    zzapp_rec_table_snapshot(&before);
    CHECK(before.free_count == 0U && before.head == 0xffffU);
    CHECK(zzapp_rec_admit(0U, &extra, &rec) == AAR_REASON_MEMORY);
    CHECK(extra == 0U);
    zzapp_rec_table_snapshot(&after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    zzapp_rec_stats(&st);
    CHECK(st.capacity == 8U && st.free == 0U && st.claimed == 8U);
    CHECK(st.refused_memory == 1U && st.refused_tables == 0U);
    /* Every admitted record holds its reserve. */
    {
        zzapp_as_stats_t as;

        zzapp_as_stats(&as);
        CHECK(as.l2_reserved == 8U * ZZAPP_AS_L2_RESERVE);
    }
}

static void test_free_order_and_fifo_reuse(void)
{
    static const uint32_t order[8] = { 3U, 0U, 6U, 1U, 7U, 2U, 5U, 4U };
    uint32_t handle[8];
    uint32_t again[8];
    uint32_t i;
    zzapp_rec_t *rec;

    setup_arena(8U);
    for (i = 0U; i < 8U; i++)
        CHECK(admit(i, &handle[i]) != NULL);
    for (i = 0U; i < 8U; i++) {
        CHECK(zzapp_rec_free(handle[order[i]]) == 0);
        CHECK(zzapp_rec_state(order[i]) == ZZAPP_REC_FREE);
    }
    /* The first free comes back first: reuse is first in, first out. */
    for (i = 0U; i < 8U; i++) {
        rec = admit(0U, &again[i]);
        CHECK(rec != NULL);
        CHECK(aar_handle_index(again[i]) == order[i]);
        /* Same index, next generation. */
        CHECK(aar_handle_generation(again[i]) == 2U);
        CHECK(aar_handle_index(handle[order[i]]) == order[i]);
    }
    /* A partial refill interleaved with new frees keeps the queue order. */
    CHECK(zzapp_rec_free(again[5]) == 0);
    CHECK(zzapp_rec_free(again[2]) == 0);
    CHECK((rec = admit(0U, &handle[0])) != NULL);
    CHECK(aar_handle_index(handle[0]) == order[5]);
    CHECK(zzapp_rec_free(again[7]) == 0);
    CHECK((rec = admit(0U, &handle[1])) != NULL);
    CHECK(aar_handle_index(handle[1]) == order[2]);
    CHECK((rec = admit(0U, &handle[2])) != NULL);
    CHECK(aar_handle_index(handle[2]) == order[7]);
    CHECK(zzapp_rec_admit(0U, &handle[3], &rec) == AAR_REASON_MEMORY);
}

/* The window index and the record index are different things. */
static void test_window_is_not_pid(void)
{
    uint32_t h[5];
    zzapp_rec_t *rec;
    uint32_t i;

    setup_arena(8U);
    for (i = 0U; i < 3U; i++)
        CHECK(admit(i, &h[i]) != NULL);
    for (i = 0U; i < 3U; i++)
        CHECK(zzapp_rec_free(h[i]) == 0);
    /* Record indices 3.. come before the freed 0..2. */
    for (i = 0U; i < 5U; i++)
        CHECK(admit(0U, &h[i]) != NULL);
    rec = zzapp_rec_by_handle(h[0]);
    CHECK(rec && rec->hdr.pid == 3U && rec->hdr.window == 0U);
    CHECK(zzapp_rec_by_handle(h[4]) && zzapp_rec_by_handle(h[4])->hdr.pid == 7U);
    CHECK(zzapp_rec_by_pid(3U) == rec);
    CHECK(aar_handle_index(h[0]) == 3U);
}

/* Generation +1 per free; 0xFFFF wraps to 1; no handle is ever 0. */
static void test_generation_wrap_and_never_zero(void)
{
    uint32_t fill[7];
    uint32_t h = 0U;
    uint32_t i;
    uint32_t expected = 1U;
    int bad_zero = 0;
    int bad_gen = 0;
    int bad_index = 0;
    zzapp_rec_table_snapshot_t snap;

    setup_arena(8U);
    /* Keep indices 1..7 busy so every admission of index 0 is this one. */
    CHECK(admit(0U, &h) != NULL);
    for (i = 0U; i < 7U; i++)
        CHECK(admit(0U, &fill[i]) != NULL);
    CHECK(aar_handle_index(h) == 0U);
    for (i = 0U; i < 70000U; i++) {
        uint32_t gen = aar_handle_generation(h);

        if (h == 0U || gen == 0U)
            bad_zero = 1;
        if (gen != expected)
            bad_gen = 1;
        if (aar_handle_index(h) != 0U)
            bad_index = 1;
        if (zzapp_rec_free(h) != 0)
            bad_gen = 1;
        expected = expected == 0xffffU ? 1U : expected + 1U;
        if (!admit(0U, &h))
            bad_gen = 1;
        if (i == 65534U) {
            /* The free of generation 0xFFFF gave generation 1 again. */
            CHECK(aar_handle_generation(h) == 1U);
        }
    }
    CHECK(!bad_zero && !bad_gen && !bad_index);
    zzapp_rec_table_snapshot(&snap);
    CHECK(snap.gen[0] == aar_handle_generation(h));
    CHECK(snap.gen[0] != 0U);
    /* No other index moved. */
    CHECK(snap.gen[1] == 1U && snap.gen[7] == 1U);
}

static void test_stale_lookups(void)
{
    uint32_t h1;
    uint32_t h2;
    uint32_t others[7];
    zzapp_rec_t *rec;
    uint32_t i;

    setup_arena(8U);
    CHECK(zzapp_rec_by_handle(0U) == NULL);
    CHECK(admit(2U, &h1) != NULL);
    CHECK(h1 != 0U);
    CHECK((rec = zzapp_rec_by_handle(h1)) != NULL);
    CHECK(rec == zzapp_rec_by_pid(aar_handle_index(h1)));
    CHECK(zzapp_rec_handle(aar_handle_index(h1)) == h1);
    /* Out of range, generation 0, wrong generation. */
    CHECK(zzapp_rec_by_handle(aar_handle_make(8U, 1U)) == NULL);
    CHECK(zzapp_rec_by_handle(aar_handle_make(0xffffU, 1U)) == NULL);
    CHECK(zzapp_rec_by_handle(aar_handle_index(h1)) == NULL);
    CHECK(zzapp_rec_by_handle(aar_handle_make(aar_handle_index(h1), 2U)) == NULL);
    CHECK(zzapp_rec_by_handle(aar_handle_make(aar_handle_index(h1),
                                              0xffffU)) == NULL);
    /* A CLAIMED and a LIVE record answer; RETIRING and RECLAIMED too (the
     * shell is still there), FREE does not. */
    CHECK(zzapp_rec_set_state(rec, ZZAPP_REC_LIVE) == 0);
    CHECK(zzapp_rec_by_handle(h1) == rec);
    CHECK(zzapp_rec_set_state(rec, ZZAPP_REC_RETIRING) == 0);
    CHECK(zzapp_rec_by_handle(h1) == rec);
    CHECK(zzapp_rec_set_state(rec, ZZAPP_REC_RECLAIMED) == 0);
    CHECK(zzapp_rec_by_handle(h1) == rec);
    CHECK(zzapp_rec_free(h1) == 0);
    {
        /* Replaying a finished free changes nothing; a handle of some other
         * life of the same index is stale. */
        zzapp_rec_table_snapshot_t once;
        zzapp_rec_table_snapshot_t twice;

        zzapp_rec_table_snapshot(&once);
        CHECK(zzapp_rec_free(h1) == 0);
        CHECK(zzapp_rec_free(aar_handle_make(aar_handle_index(h1), 7U)) == -1);
        zzapp_rec_table_snapshot(&twice);
        CHECK(memcmp(&once, &twice, sizeof(once)) == 0);
    }
    CHECK(zzapp_rec_by_handle(h1) == NULL);
    CHECK(zzapp_rec_handle(aar_handle_index(h1)) == 0U);
    /* Take every other index so the next admission reuses h1's index. */
    for (i = 0U; i < 7U; i++)
        CHECK(admit(0U, &others[i]) != NULL);
    CHECK(admit(1U, &h2) != NULL);
    CHECK(aar_handle_index(h2) == aar_handle_index(h1));
    CHECK(aar_handle_generation(h2) == aar_handle_generation(h1) + 1U);
    CHECK(zzapp_rec_by_handle(h1) == NULL);
    CHECK(zzapp_rec_by_handle(h2) == zzapp_rec_by_pid(aar_handle_index(h2)));
    /* A stale handle can neither free nor unadmit the new life. */
    CHECK(zzapp_rec_free(h1) == -1);
    CHECK(zzapp_rec_unadmit(h1) == -1);
    CHECK(zzapp_rec_state(aar_handle_index(h2)) == ZZAPP_REC_CLAIMED);
    CHECK(zzapp_rec_free(0U) == -1);
    CHECK(zzapp_rec_free(aar_handle_make(99U, 1U)) == -1);
}

/*
 * A core halted inside zzapp_rec_free(). After the generation store alone the
 * handle no longer resolves (the generation moved on) yet the record is not
 * FREE and is not in the queue: a replay with the original handle must finish
 * the free, or the record is lost for good. A handle of an earlier life of the
 * index finishes nothing, whichever store the halt stopped after.
 */
static void test_halted_free_replays_from_each_store(void)
{
    unsigned step;

    for (step = 1U; step <= 2U; step++) {
        zzapp_rec_table_snapshot_t snap;
        uint32_t old_life;
        uint32_t life;
        uint32_t others[7];
        uint32_t pid;
        uint32_t i;

        setup_arena(8U);
        CHECK(admit(0U, &old_life) != NULL);
        CHECK(zzapp_rec_free(old_life) == 0);
        /* Take every other index so the next admission reuses this one. */
        for (i = 0U; i < 7U; i++)
            CHECK(admit(0U, &others[i]) != NULL);
        CHECK(admit(1U, &life) != NULL);
        pid = aar_handle_index(life);
        CHECK(pid == aar_handle_index(old_life));
        CHECK(aar_handle_generation(life) == 2U);

        zzapp_rec_test_halt_step = step;
        CHECK(zzapp_rec_free(life) == 0);
        zzapp_rec_test_halt_step = 0U;
        zzapp_rec_table_snapshot(&snap);
        CHECK(snap.gen[pid] == 3U);
        CHECK(zzapp_rec_by_handle(life) == NULL);
        CHECK((zzapp_rec_state(pid) == ZZAPP_REC_FREE) == (step >= 2U));
        CHECK(snap.free_count == 0U);

        /* The earlier life's handle finishes nothing. */
        CHECK(zzapp_rec_free(old_life) == -1);
        CHECK((zzapp_rec_state(pid) == ZZAPP_REC_FREE) == (step >= 2U));

        /* The record is still reachable for recovery: reconcile counts it
         * busy or queues it, and a replay with the original handle ends
         * with it FREE and queued exactly once. */
        if (step == 1U)
            CHECK(zzapp_rec_reconcile_halted() == 8U);
        CHECK(zzapp_rec_free(life) == 0);
        CHECK(zzapp_rec_state(pid) == ZZAPP_REC_FREE);
        CHECK(zzapp_rec_reconcile_halted() == 7U);
        zzapp_rec_table_snapshot(&snap);
        CHECK(snap.free_count == 1U && snap.head == pid && snap.tail == pid);
        CHECK(snap.gen[pid] == 3U);
        CHECK(zzapp_rec_free(life) == 0);
    }
}

static int release_record(uint32_t handle);

/* Admission hands out a cleared record: nothing of the previous life of the
 * index survives, but the per-index constants do. */
static void test_admit_clears_the_record(void)
{
    uint32_t h[8];
    uint32_t again;
    zzapp_rec_t *rec;
    uint32_t i;

    setup_arena(8U);
    rec = admit(1U, &h[0]);
    CHECK(rec != NULL);
    rec->proc.heap_base = 0x1234U;
    rec->host.exit_code = 7U;
    rec->thread.state = ZZAPP_THREAD_RUNNABLE;
    rec->coh.running = 1U;
    rec->hdr.launch_generation = 9U;
    memset(rec->visible.block, 0xee, sizeof(rec->visible.block));
    for (i = 1U; i < 8U; i++)
        CHECK(admit(0U, &h[i]) != NULL);
    CHECK(release_record(h[0]) == 0);
    rec = admit(5U, &again);
    CHECK(rec == zzapp_rec_by_handle(again) && rec->hdr.pid == 0U);
    CHECK(rec->proc.heap_base == 0U && rec->host.exit_code == 0U);
    CHECK(rec->thread.state == ZZAPP_THREAD_STOPPED && rec->coh.running == 0U);
    CHECK(rec->hdr.launch_generation == 0U && rec->hdr.window == 5U);
    CHECK(rec->visible.block[3].bytes == 0U && rec->visible.block[63].card == 0U);
    CHECK(rec->hdr.kstack_top == zzapp_rec_stack_top(0U));
    CHECK(rec->hdr.l1 == (uint32_t)(uintptr_t)zzapp_as_pool_l1(0U));
    CHECK(rec->hdr.retire_stage == ZZAPP_RETIRE_NONE);
}

static void test_state_transitions(void)
{
    uint32_t h;
    zzapp_rec_t *rec;

    setup_arena(8U);
    rec = admit(1U, &h);
    CHECK(rec != NULL);
    CHECK(zzapp_rec_set_state(rec, ZZAPP_REC_FREE) == -1);
    CHECK(zzapp_rec_set_state(rec, (zzapp_rec_state_t)9) == -1);
    CHECK(zzapp_rec_set_state(rec, ZZAPP_REC_LIVE) == 0);
    CHECK(zzapp_rec_set_state(rec, ZZAPP_REC_LIVE) == 0);
    CHECK(zzapp_rec_set_state(rec, ZZAPP_REC_CLAIMED) == -1);
    CHECK(zzapp_rec_set_state(rec, ZZAPP_REC_RETIRING) == 0);
    CHECK(zzapp_rec_set_state(rec, ZZAPP_REC_LIVE) == -1);
    CHECK(zzapp_rec_state(aar_handle_index(h)) == ZZAPP_REC_RETIRING);
    CHECK(zzapp_rec_set_state(NULL, ZZAPP_REC_LIVE) == -1);
    CHECK(zzapp_rec_free(h) == 0);
    /* A FREE record takes no transition. */
    CHECK(zzapp_rec_set_state(rec, ZZAPP_REC_LIVE) == -1);
    CHECK(zzapp_rec_set_state(rec, ZZAPP_REC_RETIRING) == -1);
}

static void test_owner_serial(void)
{
    uint32_t h[8];
    uint32_t seen[8];
    zzapp_rec_t *rec;
    uint32_t i;
    uint32_t j;

    setup_arena(8U);
    for (i = 0U; i < 8U; i++) {
        rec = admit(0U, &h[i]);
        CHECK(rec != NULL);
        seen[i] = rec ? rec->hdr.owner_serial : 0U;
        CHECK(seen[i] != 0U && seen[i] <= 0x00ffffffU);
        for (j = 0U; j < i; j++)
            CHECK(seen[i] != seen[j]);
    }
    /* Not reused after a free either. */
    CHECK(zzapp_rec_free(h[3]) == 0);
    rec = admit(0U, &h[0]);
    CHECK(rec != NULL && rec->hdr.owner_serial > seen[7]);
}

/* ---- Admission refusal and unadmit ------------------------------------- */

/* What a finished retire leaves: tables and reserve returned, then FREE. */
static int release_record(uint32_t handle)
{
    zzapp_rec_t *rec = zzapp_rec_by_handle(handle);

    if (!rec)
        return -1;
    zzapp_rec_retire_tables(rec);
    return zzapp_rec_free(handle);
}

static void test_unadmit_is_bit_identical(void)
{
    zzapp_rec_table_snapshot_t before;
    zzapp_rec_table_snapshot_t after;
    zzapp_as_stats_t as_before;
    zzapp_as_stats_t as_after;
    zzapp_rec_stats_t st_before;
    zzapp_rec_stats_t st_after;
    uint32_t h[8];
    uint32_t t;
    zzapp_rec_t *rec;
    uint32_t i;

    setup_arena(8U);
    CHECK(admit(0U, &h[0]) != NULL);
    CHECK(admit(0U, &h[1]) != NULL);
    CHECK(release_record(h[0]) == 0);          /* queue now 2 .. 7, 0 */
    zzapp_rec_table_snapshot(&before);
    zzapp_as_stats(&as_before);
    zzapp_rec_stats(&st_before);
    rec = admit(5U, &t);
    CHECK(rec != NULL);
    CHECK(aar_handle_index(t) == 2U);
    CHECK(zzapp_rec_unadmit(t) == 0);
    zzapp_rec_table_snapshot(&after);
    zzapp_as_stats(&as_after);
    zzapp_rec_stats(&st_after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    CHECK(memcmp(&as_before, &as_after, sizeof(as_before)) == 0);
    CHECK(memcmp(&st_before, &st_after, sizeof(st_before)) == 0);
    CHECK(rec && rec->hdr.handle == 0U && rec->hdr.window == ZZAPP_REC_NO_WINDOW);
    CHECK(zzapp_rec_by_handle(t) == NULL);
    /* The next admission gets the very same handle and serial. */
    {
        uint32_t again;
        zzapp_rec_t *r2 = admit(5U, &again);

        CHECK(r2 == rec && again == t);
        CHECK(r2->hdr.owner_serial == 3U);
        CHECK(zzapp_rec_unadmit(again) == 0);
    }
    /* The last free index: the queue empties and refills as it was. */
    for (i = 2U; i < 8U; i++)
        CHECK(admit(0U, &h[i]) != NULL);
    CHECK(admit(0U, &h[0]) != NULL);           /* the freed 0 is last */
    zzapp_rec_table_snapshot(&before);
    zzapp_as_stats(&as_before);
    CHECK(before.free_count == 0U);
    CHECK(release_record(h[2]) == 0);
    zzapp_rec_table_snapshot(&before);
    zzapp_as_stats(&as_before);
    CHECK(before.free_count == 1U && before.head == before.tail);
    rec = admit(3U, &t);
    CHECK(rec != NULL);
    CHECK(zzapp_rec_unadmit(t) == 0);
    zzapp_rec_table_snapshot(&after);
    zzapp_as_stats(&as_after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    CHECK(memcmp(&as_before, &as_after, sizeof(as_before)) == 0);
    /* Only a CLAIMED record can be unadmitted. */
    rec = admit(3U, &t);
    CHECK(rec != NULL);
    CHECK(zzapp_rec_set_state(rec, ZZAPP_REC_LIVE) == 0);
    CHECK(zzapp_rec_unadmit(t) == -1);
    CHECK(zzapp_rec_state(aar_handle_index(t)) == ZZAPP_REC_LIVE);
    CHECK(zzapp_rec_unadmit(0U) == -1);
}

/* A reserve that does not fit refuses with TABLES and changes nothing. */
static void test_tables_refusal(void)
{
    zzapp_rec_layout_t lay;
    uint32_t addr;
    uint32_t h[8];
    uint32_t refused = 0U;
    zzapp_rec_t *rec;
    zzapp_rec_table_snapshot_t before;
    zzapp_rec_table_snapshot_t after;
    zzapp_as_stats_t as_before;
    zzapp_as_stats_t as_after;
    zzapp_rec_stats_t st;

    host_card_pool_reset(64U << 20);
    zzapp_rec_layout_for(8U, HEAP_78M, &lay);
    /* Tables for two reserves and nothing else. */
    lay.l2_total = 2U * ZZAPP_AS_L2_RESERVE + 8U;
    lay.as_bytes = zzapp_as_pool_bytes(lay.apps, lay.l2_total);
    addr = card_pool_alloc_aligned(&card_pool, lay.bytes, ZZAPP_REC_ARENA_ALIGN,
        CARD_POOL_OWNER(CARD_POOL_CLASS_APP_ARENA, 0U));
    CHECK(zzapp_rec_arena_init((void *)(uintptr_t)addr, &lay) == 0);
    load_bsp();
    zzapp_as_init(bsp);
    CHECK(admit(0U, &h[0]) != NULL);
    CHECK(admit(1U, &h[1]) != NULL);
    zzapp_rec_table_snapshot(&before);
    zzapp_as_stats(&as_before);
    CHECK(zzapp_rec_admit(2U, &refused, &rec) == AAR_REASON_TABLES);
    CHECK(refused == 0U);
    zzapp_rec_table_snapshot(&after);
    zzapp_as_stats(&as_after);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    CHECK(memcmp(&as_before, &as_after, sizeof(as_before)) == 0);
    zzapp_rec_stats(&st);
    CHECK(st.refused_tables == 1U && st.refused_memory == 0U);
    CHECK(after.free_count == 6U && after.head == 2U);
    /* A record that retires returns its reserve: the next claim fits. */
    rec = zzapp_rec_by_handle(h[0]);
    CHECK(rec != NULL);
    zzapp_rec_retire_tables(rec);
    CHECK(zzapp_rec_free(h[0]) == 0);
    CHECK(admit(2U, &h[2]) != NULL);
    CHECK(aar_handle_index(h[2]) == 2U);
}

/* ---- Retire halts and reconcile ---------------------------------------- */

#define HALT_NONE 0U

typedef struct {
    zzapp_rec_t *rec;
    uint32_t handle;
} life_t;

/* Admit a record and give it what a running app holds: an address space with
 * mapped alias pages (L2 tables), a stage marker and a thread state. */
static life_t start_life(uint32_t window)
{
    life_t life;
    uint32_t va;

    life.rec = admit(window, &life.handle);
    CHECK(life.rec != NULL);
    if (!life.rec)
        return life;
    CHECK(zzapp_as_create(&life.rec->proc.as, life.rec->hdr.pid) == 0);
    CHECK(zzapp_as_alias_map(&life.rec->proc.as, 0x03400000U, 5U << 20, 1,
                             &va) == 0);
    CHECK(zzapp_rec_set_state(life.rec, ZZAPP_REC_LIVE) == 0);
    zzapp_as_switch(&life.rec->proc.as);
    return life;
}

/*
 * The retire sequence a halted core could be interrupted in, in the order of
 * the design: stage 1 (RETIRING), stage 4 (tables), stage 7 (free). `stop` is
 * the number of completed steps after which the core halts (0 = runs through):
 *   1 RETIRING + DETACHED set
 *   2 retire_tables step 1 (space destroyed)
 *   3 retire_tables step 2 (reserve released)
 *   4 retire_tables done (stage marked)
 *   5 free step 1 (generation bumped)
 *   6 free step 2 (FREE published)
 *   7 free done (queued)
 */
static void run_retire(life_t *life, uint32_t stop)
{
    zzapp_rec_t *rec = life->rec;

    zzapp_rec_test_halt_step = 0U;
    if (zzapp_rec_state(rec->hdr.pid) != ZZAPP_REC_FREE)
        CHECK(zzapp_rec_set_state(rec, ZZAPP_REC_RETIRING) == 0);
    zzapp_rec_set_stage(rec, ZZAPP_RETIRE_DETACHED);
    if (stop == 1U)
        return;
    zzapp_rec_test_halt_step = stop == 2U ? 1U : stop == 3U ? 2U : 0U;
    zzapp_rec_retire_tables(rec);
    zzapp_rec_test_halt_step = 0U;
    if (stop >= 2U && stop <= 4U)
        return;
    zzapp_rec_test_halt_step = stop == 5U ? 1U : stop == 6U ? 2U : 0U;
    (void)zzapp_rec_free(life->handle);
    zzapp_rec_test_halt_step = 0U;
}

static void test_reconcile_after_halt_at_each_stage(void)
{
    uint32_t stop;

    for (stop = 1U; stop <= 7U; stop++) {
        zzapp_rec_table_snapshot_t base;
        zzapp_rec_table_snapshot_t snap;
        zzapp_as_stats_t as_base;
        zzapp_as_stats_t as_now;
        zzapp_as_stats_t as_mid;
        zzapp_rec_stats_t st;
        life_t victim;
        life_t other;
        uint32_t busy;
        uint32_t victim_pid;
        uint32_t i;
        uint32_t chain = 0U;
        uint8_t seen[8] = { 0 };

        setup_arena(8U);
        zzapp_rec_table_snapshot(&base);
        zzapp_as_stats(&as_base);
        victim = start_life(0U);
        other = start_life(1U);
        victim_pid = victim.rec ? victim.rec->hdr.pid : 0U;
        /* The life that stays: its tables must survive every repair. */
        zzapp_as_stats(&as_now);
        CHECK(as_now.l2_free == as_base.l2_free - 10U);

        run_retire(&victim, stop);

        /* Core 1 restarts: reconcile, then the retire runs again from the
         * top, as a restarted core replays the recorded stage. */
        /* A halt in the middle of a table operation can leave the pool's
         * counters and lists torn; the owner marks are the truth. */
        zzapp_as_stats(&as_mid);
        zzapp_as_test_scramble_pool();
        busy = zzapp_rec_reconcile_halted();
        zzapp_as_stats(&as_now);
        CHECK(memcmp(&as_mid, &as_now, sizeof(as_mid)) == 0);
        zzapp_rec_table_snapshot(&snap);
        /* Every FREE index is in the queue exactly once, and only those. */
        for (i = snap.head; i < 8U && chain <= 8U; i = snap.next[i], chain++) {
            CHECK(!seen[i]);
            seen[i] = 1U;
            CHECK((snap.state[i] & 0x7fU) == ZZAPP_REC_FREE);
        }
        CHECK(chain == snap.free_count);
        {
            uint32_t free_states = 0U;

            for (i = 0U; i < 8U; i++)
                free_states += (snap.state[i] & 0x7fU) == ZZAPP_REC_FREE;
            CHECK(free_states == snap.free_count);
            CHECK(busy == 8U - free_states);
        }
        /* The other life is untouched. */
        CHECK(zzapp_rec_by_handle(other.handle) == other.rec);
        CHECK(zzapp_as_asid_valid(&other.rec->proc.as));
        CHECK(zzapp_as_alias_phys(&other.rec->proc.as,
                                  ZZAPP_AS_ALIAS_BASE) == 0x03400000U);

        run_retire(&victim, 0U);
        zzapp_rec_stats(&st);
        CHECK(zzapp_rec_state(victim_pid) == ZZAPP_REC_FREE);
        CHECK(zzapp_rec_by_handle(victim.handle) == NULL);
        CHECK(st.free == 7U && st.live == 1U);
        zzapp_as_stats(&as_now);
        CHECK(as_now.l2_free == as_base.l2_free - 5U);
        CHECK(as_now.l2_reserved == ZZAPP_AS_L2_RESERVE);
        CHECK(!victim.rec->proc.as.active);
        CHECK(zzapp_rec_reconcile_halted() == 1U);
        zzapp_rec_table_snapshot(&snap);
        CHECK(snap.free_count == 7U);
        /* The victim's index is back in the queue, generation moved on by
         * exactly one free. */
        CHECK(snap.gen[victim_pid] == 2U);
        /* The survivor retires too: everything is back to the baseline. */
        run_retire(&other, 0U);
        zzapp_as_stats(&as_now);
        CHECK(as_now.l2_free == as_base.l2_free && as_now.l2_reserved == 0U &&
              as_now.l2_reserved_unused == 0U && as_now.asid_assigned == 0U);
        zzapp_rec_table_snapshot(&snap);
        CHECK(snap.free_count == 8U);
        for (i = 0U; i < 8U; i++)
            CHECK((snap.state[i] & 0xffU) == ZZAPP_REC_FREE);
        (void)base;
    }
}

/* Reconcile keeps the order of an intact queue and repairs a torn one. */
static void test_reconcile_queue_order(void)
{
    uint32_t h[8];
    uint32_t i;
    zzapp_rec_table_snapshot_t snap;
    uint32_t walk[8];
    uint32_t n = 0U;
    uint32_t idx;
    static const uint32_t order[4] = { 5U, 2U, 7U, 0U };

    setup_arena(8U);
    for (i = 0U; i < 8U; i++)
        CHECK(admit(0U, &h[i]) != NULL);
    for (i = 0U; i < 4U; i++)
        CHECK(zzapp_rec_free(h[order[i]]) == 0);
    CHECK(zzapp_rec_reconcile_halted() == 4U);
    zzapp_rec_table_snapshot(&snap);
    for (idx = snap.head; idx < 8U && n < 8U; idx = snap.next[idx])
        walk[n++] = idx;
    CHECK(n == 4U);
    for (i = 0U; i < 4U && i < n; i++)
        CHECK(walk[i] == order[i]);
    CHECK(snap.tail == order[3] && snap.next[order[3]] == 0xffffU);
    /* Halt between FREE and the queue: the lost index joins at the end, in
     * ascending order with any other lost one. */
    zzapp_rec_test_halt_step = 2U;
    CHECK(zzapp_rec_free(h[6]) == 0);
    CHECK(zzapp_rec_free(h[1]) == 0);
    zzapp_rec_test_halt_step = 0U;
    zzapp_rec_table_snapshot(&snap);
    CHECK(snap.free_count == 4U);
    CHECK(zzapp_rec_reconcile_halted() == 2U);
    zzapp_rec_table_snapshot(&snap);
    CHECK(snap.free_count == 6U);
    n = 0U;
    for (idx = snap.head; idx < 8U && n < 8U; idx = snap.next[idx])
        walk[n++] = idx;
    CHECK(n == 6U);
    CHECK(walk[0] == 5U && walk[1] == 2U && walk[2] == 7U && walk[3] == 0U);
    CHECK(walk[4] == 1U && walk[5] == 6U);
    /* A damaged chain (a cycle) is rebuilt, not followed forever. */
    zzapp_rec_test_poke_next(0U, 5U);
    CHECK(zzapp_rec_reconcile_halted() == 2U);
    zzapp_rec_table_snapshot(&snap);
    CHECK(snap.free_count == 6U);
    CHECK(snap.next[snap.tail] == 0xffffU);
    /* Idempotent. */
    {
        zzapp_rec_table_snapshot_t again;

        CHECK(zzapp_rec_reconcile_halted() == 2U);
        zzapp_rec_table_snapshot(&again);
        CHECK(memcmp(&snap, &again, sizeof(snap)) == 0);
    }
}

/* A restarted core 1 loses the zzapp_as_t struct; retire still finds the
 * space by index. */
static void test_retire_tables_finds_a_lost_space(void)
{
    life_t life;
    zzapp_as_stats_t base;
    zzapp_as_stats_t now;

    setup_arena(8U);
    zzapp_as_stats(&base);
    life = start_life(0U);
    CHECK(life.rec != NULL);
    memset(&life.rec->proc.as, 0, sizeof(life.rec->proc.as));
    run_retire(&life, 0U);
    zzapp_as_stats(&now);
    CHECK(now.l2_free == base.l2_free && now.l2_reserved == 0U);
    CHECK(now.asid_assigned == 0U);
    CHECK(zzapp_rec_state(life.rec->hdr.pid) == ZZAPP_REC_FREE);
    CHECK(life.rec->hdr.retire_stage == ZZAPP_RETIRE_TABLES);
    /* The stage marker only ever advances. */
    zzapp_rec_set_stage(life.rec, ZZAPP_RETIRE_DETACHED);
    CHECK(life.rec->hdr.retire_stage == ZZAPP_RETIRE_TABLES);
    zzapp_rec_set_stage(life.rec, ZZAPP_RETIRE_WIRE);
    CHECK(life.rec->hdr.retire_stage == ZZAPP_RETIRE_WIRE);
}

/* The record layer drives the table pool: every L2 table a record held
 * comes back through the retire, so a full lap of records leaks nothing. */
static void test_retire_returns_every_table(void)
{
    uint32_t round;
    zzapp_as_stats_t base;
    zzapp_as_stats_t now;
    life_t life[8];
    uint32_t i;

    setup_arena(8U);
    zzapp_as_stats(&base);
    for (round = 0U; round < 3U; round++) {
        for (i = 0U; i < 8U; i++)
            life[i] = start_life(i);
        zzapp_as_stats(&now);
        CHECK(now.l2_reserved == 8U * ZZAPP_AS_L2_RESERVE);
        CHECK(now.asid_assigned == 8U);
        for (i = 0U; i < 8U; i++)
            run_retire(&life[round & 1U ? 7U - i : i], 0U);
        zzapp_as_stats(&now);
        CHECK(now.l2_free == base.l2_free && now.l2_reserved == 0U &&
              now.l2_reserved_unused == 0U && now.asid_assigned == 0U);
    }
}

int main(void)
{
    test_sizing();
    test_arena_class();
    test_carve();
    test_arena_init_validation();
    test_boot_clears_records();
    test_index_areas();
    test_capacity_refusal();
    test_free_order_and_fifo_reuse();
    test_window_is_not_pid();
    test_generation_wrap_and_never_zero();
    test_stale_lookups();
    test_halted_free_replays_from_each_store();
    test_admit_clears_the_record();
    test_state_transitions();
    test_owner_serial();
    test_unadmit_is_bit_identical();
    test_tables_refusal();
    test_reconcile_after_halt_at_each_stage();
    test_reconcile_queue_order();
    test_retire_tables_finds_a_lost_space();
    test_retire_returns_every_table();
    printf("zzapp_rec: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
