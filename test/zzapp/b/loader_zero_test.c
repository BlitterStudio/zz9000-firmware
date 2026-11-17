/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "card_pool.h"
#include "host_card_pool.h"
#include "zzapp_mem.h"

#define MB(n) ((uint32_t)(n) << 20)
#define ZERO_CHUNK_FRAMES 2U

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

static void reset(zzapp_mem_t *memory, zzapp_mem_proc_t *proc)
{
    host_card_pool_reset(MB(256));
    assert(zzapp_mem_init(memory, &card_pool) == 0);
    zzapp_mem_proc_init(proc, MB(96));
}

static void test_legacy_zeroing_is_bounded_and_complete(void)
{
    zzapp_mem_t memory;
    zzapp_mem_proc_t proc;
    uint32_t frames[96];
    uint32_t dirty;
    uint32_t first = 0U;
    uint32_t passes = 0U;
    uint32_t frame;

    reset(&memory, &proc);
    dirty = card_pool_alloc_aligned(&card_pool, MB(96), MB(1),
                                    CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA, 1));
    assert(dirty != 0U);
    memset((void *)(uintptr_t)dirty, 0xa5, MB(96));
    assert(card_pool_free(&card_pool, dirty,
                          CARD_POOL_OWNER(CARD_POOL_CLASS_MEDIA, 1)) == 1);
    assert(zzapp_mem_alloc_unzeroed(&memory, &proc, 96U, frames) == 0);
    assert(*(const uint32_t *)(uintptr_t)frames[0] == 0xa5a5a5a5U);
    while (first != 96U) {
        uint32_t zeroed = zzapp_mem_zero_frames(&proc, first,
                                                 ZERO_CHUNK_FRAMES);

        assert(zeroed != 0U && zeroed <= ZERO_CHUNK_FRAMES);
        first += zeroed;
        passes++;
    }
    assert(passes > 1U);
    for (frame = 0U; frame < 96U; frame++)
        assert(memchr((void *)(uintptr_t)frames[frame], 0xa5, MB(1)) == NULL);
}

static void test_cancelling_mid_zeroing_releases_every_frame(void)
{
    zzapp_mem_t memory;
    zzapp_mem_proc_t proc;
    uint32_t frames[96];
    uint32_t free_before;

    reset(&memory, &proc);
    free_before = card_pool_range_free_bytes(&card_pool, 0);
    assert(zzapp_mem_alloc_unzeroed(&memory, &proc, 96U, frames) == 0);
    assert(zzapp_mem_zero_frames(&proc, 0U, ZERO_CHUNK_FRAMES) ==
           ZERO_CHUNK_FRAMES);
    assert(zzapp_mem_release_proc(&memory, &proc) == MB(96));
    assert(proc.frame_count == 0U);
    assert(memory.committed == 0U);
    assert(card_pool_range_free_bytes(&card_pool, 0) == free_before);
}

int main(void)
{
    test_legacy_zeroing_is_bounded_and_complete();
    test_cancelling_mid_zeroing_releases_every_frame();
    return 0;
}
