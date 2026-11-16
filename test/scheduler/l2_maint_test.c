#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "l2_maint.h"
#include "sdk_smp_lock.h"

typedef intptr_t INTPTR;
typedef uint32_t u32;

/*
 * A small write-back cache model: one L1 and one L2 copy per 32-byte line
 * in front of a DDR array.  CPU stores land dirty in L1, a device (DMA)
 * writes DDR directly.  The wrapper's platform hooks act on this model, so
 * the tests check what software actually observes afterwards, not which
 * hooks were called.
 */
#define LINE 32U
#define BASE 0x1000U
#define LINES 10240U
#define BYTES (LINES * LINE)

#define L2_REG_CACHE_SYNC       0x0730U
#define L2_REG_CACHE_INVLD_PA   0x0770U
#define L2_REG_CACHE_CLEAN_PA   0x07B0U
#define L2_REG_CACHE_INV_CLN_PA 0x07F0U
#define L2_REG_DEBUG_CTRL       0x0F40U
#define MAX_L2_WRITES (LINES + LINES / (L2_MAINT_CHUNK_BYTES / LINE) + 2U)

struct l2_write {
    u32 offset;
    u32 value;
};

struct line {
    int valid;
    int dirty;
    uint8_t data[LINE];
};

static uint8_t ddr[BYTES];
static struct line l1[LINES];
static struct line l2[LINES];

static int test_cpu;
static uint32_t irq_masked;
static unsigned l2_clean_calls;
static unsigned l2_clean_inv_calls;
static unsigned l2_inv_calls;
static unsigned l2_syncs;
static unsigned l2_debug_writes;
static unsigned syncs_at_lock_acquire;
static struct l2_write l2_writes[MAX_L2_WRITES];
static unsigned l2_write_count;
static INTPTR l2_clean_addresses[LINES];
static INTPTR l2_inv_addresses[LINES];
static INTPTR l2_clean_inv_addresses[LINES];
static unsigned l2_ops_unlocked;
static unsigned lock_holds;
static unsigned max_bytes_per_hold;
static unsigned bytes_this_hold;

int smp_cpu_id(void) { return test_cpu; }
uint32_t smp_local_irq_save(void)
{
    uint32_t previous = irq_masked;
    irq_masked = 1U;
    return previous;
}
void smp_local_irq_restore(uint32_t state) { irq_masked = state; }
void smp_raw_spin_lock(volatile uint32_t *word)
{
    assert(*word == 0U);
    *word = 1U;
    lock_holds++;
    bytes_this_hold = 0U;
    syncs_at_lock_acquire = l2_syncs;
}
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word != 0U)
        return 0;
    *word = 1U;
    lock_holds++;
    bytes_this_hold = 0U;
    syncs_at_lock_acquire = l2_syncs;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word)
{
    assert(*word == 1U);
    if (bytes_this_hold != 0U)
        assert(l2_syncs > syncs_at_lock_acquire);
    *word = 0U;
}

static unsigned index_of(INTPTR address)
{
    assert(address >= (INTPTR)BASE && address < (INTPTR)(BASE + BYTES));
    assert(((u32)address & (LINE - 1U)) == 0U);
    return (unsigned)(((u32)address - BASE) / LINE);
}

static void l2_op_locked(void)
{
    if (irq_masked == 0U)
        l2_ops_unlocked++;
    bytes_this_hold += LINE;
    if (bytes_this_hold > max_bytes_per_hold)
        max_bytes_per_hold = bytes_this_hold;
}

/* CPU view: L1, then L2, then DDR, with line fills on a miss. */
static struct line *cpu_line(u32 address)
{
    unsigned i = index_of((INTPTR)(address & ~(LINE - 1U)));

    if (!l1[i].valid) {
        if (!l2[i].valid) {
            memcpy(l2[i].data, &ddr[i * LINE], LINE);
            l2[i].valid = 1;
            l2[i].dirty = 0;
        }
        memcpy(l1[i].data, l2[i].data, LINE);
        l1[i].valid = 1;
        l1[i].dirty = 0;
    }
    return &l1[i];
}

static void cpu_store(u32 address, uint8_t value)
{
    struct line *line = cpu_line(address);

    line->data[address & (LINE - 1U)] = value;
    line->dirty = 1;
}

static uint8_t cpu_load(u32 address)
{
    return cpu_line(address)->data[address & (LINE - 1U)];
}

static void l1_clean_inv(unsigned i)
{
    if (l1[i].valid && l1[i].dirty) {
        memcpy(l2[i].data, l1[i].data, LINE);
        l2[i].valid = 1;
        l2[i].dirty = 1;
    }
    l1[i].valid = 0;
    l1[i].dirty = 0;
}

static void l2_clean_inv(unsigned i)
{
    if (l2[i].valid && l2[i].dirty)
        memcpy(&ddr[i * LINE], l2[i].data, LINE);
    l2[i].valid = 0;
    l2[i].dirty = 0;
}

static void l1_clean(unsigned i)
{
    if (l1[i].valid && l1[i].dirty) {
        memcpy(l2[i].data, l1[i].data, LINE);
        l2[i].valid = 1;
        l2[i].dirty = 1;
    }
    l1[i].dirty = 0;
}

void l2_maint_platform_l1_flush_range(INTPTR address, u32 length)
{
    u32 a = (u32)address & ~(LINE - 1U);
    u32 end = (u32)address + length;

    for (; a < end; a += LINE)
        l1_clean_inv(index_of((INTPTR)a));
}
void l2_maint_platform_l1_clean_range(INTPTR address, u32 length)
{
    u32 a = (u32)address & ~(LINE - 1U);
    u32 end = (u32)address + length;

    for (; a < end; a += LINE)
        l1_clean(index_of((INTPTR)a));
}

void l2_maint_platform_l1_flush(void)
{
    unsigned i;

    for (i = 0U; i < LINES; i++)
        l1_clean_inv(i);
}
void l2_maint_platform_l1_clean_inv_line(INTPTR address)
{
    l1_clean_inv(index_of(address));
}
void l2_maint_platform_l1_inv_line(INTPTR address)
{
    unsigned i = index_of(address);

    l1[i].valid = 0;
    l1[i].dirty = 0;
}
static void l2_clean(unsigned i)
{
    if (l2[i].valid && l2[i].dirty)
        memcpy(&ddr[i * LINE], l2[i].data, LINE);
    l2[i].dirty = 0;
}

void l2_maint_platform_l2_write(u32 offset, u32 value)
{
    unsigned i;

    assert(l2_write_count < MAX_L2_WRITES);
    l2_writes[l2_write_count++] = (struct l2_write){ offset, value };
    switch (offset) {
    case L2_REG_CACHE_CLEAN_PA:
        i = index_of((INTPTR)value);
        l2_op_locked();
        l2_clean_addresses[l2_clean_calls++] = (INTPTR)value;
        l2_clean(i);
        break;
    case L2_REG_CACHE_INV_CLN_PA:
        i = index_of((INTPTR)value);
        l2_op_locked();
        l2_clean_inv_addresses[l2_clean_inv_calls++] = (INTPTR)value;
        l2_clean_inv(i);
        break;
    case L2_REG_CACHE_INVLD_PA:
        i = index_of((INTPTR)value);
        l2_op_locked();
        l2_inv_addresses[l2_inv_calls++] = (INTPTR)value;
        l2[i].valid = 0;
        l2[i].dirty = 0;
        break;
    case L2_REG_CACHE_SYNC:
        if (irq_masked == 0U)
            l2_ops_unlocked++;
        l2_syncs++;
        break;
    case L2_REG_DEBUG_CTRL:
        l2_debug_writes++;
        break;
    default:
        assert(0);
    }
}

void l2_maint_platform_l2_flush(void)
{
    unsigned i;

    if (irq_masked == 0U)
        l2_ops_unlocked++;
    for (i = 0U; i < LINES; i++)
        l2_clean_inv(i);
}

/* The model has no separate I-cache: invalidating the core's L1 I-side
 * only has to leave L1 D and L2 untouched. */
static unsigned l1_icache_invalidates;
void l2_maint_platform_l1_icache_invalidate_range(INTPTR address, u32 length)
{
    (void)address;
    (void)length;
    l1_icache_invalidates++;
}

/* An instruction fetch after an L1 I-cache miss reads the unified L2,
 * then DDR. */
static uint8_t fetch(u32 address)
{
    unsigned i = index_of((INTPTR)(address & ~(LINE - 1U)));

    if (l2[i].valid)
        return l2[i].data[address & (LINE - 1U)];
    return ddr[address - BASE];
}

static void reset_model(void)
{
    memset(ddr, 0, sizeof(ddr));
    memset(l1, 0, sizeof(l1));
    memset(l2, 0, sizeof(l2));
    l2_clean_calls = 0U;
    l2_clean_inv_calls = 0U;
    l2_inv_calls = 0U;
    l2_syncs = 0U;
    l2_debug_writes = 0U;
    l2_ops_unlocked = 0U;
    lock_holds = 0U;
    max_bytes_per_hold = 0U;
    bytes_this_hold = 0U;
    syncs_at_lock_acquire = 0U;
    l2_write_count = 0U;
}

/* Each 4 KiB serialized PL310 batch covers every line, then syncs once. */
static void test_invalidate_batches_sync(void)
{
    const u32 buf = 0x1000U;
    const u32 len = L2_MAINT_CHUNK_BYTES * 2U + LINE;
    unsigned i;

    reset_model();
    l2_maint_dcache_invalidate_range((INTPTR)buf, len);
    assert(l2_clean_inv_calls == 0U);
    assert(l2_inv_calls == len / LINE);
    for (i = 0U; i < l2_inv_calls; i++)
        assert(l2_inv_addresses[i] == (INTPTR)(buf + i * LINE));
    assert(l2_syncs == 3U);
    assert(max_bytes_per_hold <= L2_MAINT_CHUNK_BYTES);
    assert(irq_masked == 0U);
}

/* A DMA receive buffer that starts and ends mid-line, with CPU-owned dirty
 * data in the same lines just outside it (the boot-time SD fault: a static
 * next to a buffer reverted to power-on DDR contents). */
static void test_invalidate_keeps_neighbours(void)
{
    const u32 buf = 0x2011U;
    const u32 len = 0x1FC0U;    /* ends mid-line at 0x3FD1 */
    const u32 before = buf - 1U;
    const u32 after = buf + len;
    u32 a;

    reset_model();
    /* Stale cached copies of the whole region, plus dirty neighbours. */
    for (a = buf - 0x11U; a < after + 0x2FU; a++)
        (void)cpu_load(a);
    cpu_store(before, 0xA5U);
    cpu_store(after, 0x5AU);
    /* Some lines also sit dirty-but-evicted in L2 only. */
    l1_clean_inv(index_of(0x2400));
    /* The device writes the whole lines inside the buffer to DDR. */
    for (a = (buf + LINE - 1U) & ~(LINE - 1U); a < (after & ~(LINE - 1U)); a++)
        ddr[a - BASE] = (uint8_t)(a * 7U);

    l2_maint_dcache_invalidate_range((INTPTR)buf, len);
    assert(cpu_load(before) == 0xA5U);
    assert(cpu_load(after) == 0x5AU);
    for (a = (buf + LINE - 1U) & ~(LINE - 1U); a < (after & ~(LINE - 1U)); a++)
        assert(cpu_load(a) == (uint8_t)(a * 7U));
    assert(l2_clean_inv_calls == 2U);
    assert(l2_clean_inv_addresses[0] == (INTPTR)0x2000U);
    assert(l2_clean_inv_addresses[1] == (INTPTR)0x3FC0U);
    assert(l2_syncs == 4U);
    assert(l2_ops_unlocked == 0U);
    assert(max_bytes_per_hold <= L2_MAINT_CHUNK_BYTES);
    assert(irq_masked == 0U);
}

/* A range inside a single line must clean it, not drop the neighbours. */
static void test_invalidate_within_one_line(void)
{
    reset_model();
    cpu_store(0x5000U, 0x11U);
    cpu_store(0x5004U, 0x22U);
    cpu_store(0x501FU, 0x33U);
    l2_maint_dcache_invalidate_range((INTPTR)0x5004U, 4U);
    assert(cpu_load(0x5000U) == 0x11U);
    assert(cpu_load(0x501FU) == 0x33U);
    assert(l2_clean_inv_calls == 1U);
    assert(l2_clean_inv_addresses[0] == (INTPTR)0x5000U);
    assert(l2_inv_calls == 0U);
    assert(l2_syncs == 1U);
    assert(l2_ops_unlocked == 0U);
}

/* The L2-only variant used for DMA lines has the same edge rule. */
static void test_l2_invalidate_keeps_neighbours(void)
{
    reset_model();
    cpu_store(0x6008U, 0x77U);
    l1_clean_inv(index_of(0x6000));
    l2_maint_l2_invalidate_range((INTPTR)0x6010U, 0x30U);
    assert(cpu_load(0x6008U) == 0x77U);
    assert(l2_clean_inv_calls == 1U);
    assert(l2_clean_inv_addresses[0] == (INTPTR)0x6000U);
    assert(l2_inv_calls == 1U);
    assert(l2_inv_addresses[0] == (INTPTR)0x6020U);
    assert(l2_syncs == 2U);
    assert(l2_ops_unlocked == 0U);
}

/*
 * Scanout consumes DDR through the HP port, so L1 and L2 clean is sufficient:
 * the surface stays resident in both caches, without sacrificing display
 * coherency.  One 640x480 CLUT8 frame must not add per-line debug writes or
 * more than one serialized batch per 4 KiB.
 */
static void test_present_clean_reaches_ddr(void)
{
    const u32 buf = BASE;
    const u32 len = 640U * 480U;
    const unsigned batches = len / L2_MAINT_CHUNK_BYTES;
    u32 a;
    unsigned i;
    unsigned clean_writes = 0U;
    unsigned sync_writes = 0U;

    reset_model();
    for (a = buf; a < buf + len; a++)
        cpu_store(a, (uint8_t)(a * 5U));
    l2_maint_dcache_clean_range((INTPTR)buf, len);
    for (a = buf; a < buf + len; a++)
        assert(ddr[a - BASE] == (uint8_t)(a * 5U));
    assert(l2_clean_calls == len / LINE);
    assert(l2_clean_inv_calls == 0U);
    assert(l2_inv_calls == 0U);
    for (i = 0U; i < l2_write_count; i++) {
        if (l2_writes[i].offset == L2_REG_CACHE_CLEAN_PA) {
            assert(l2_writes[i].value == buf + clean_writes * LINE);
            clean_writes++;
        } else if (l2_writes[i].offset == L2_REG_CACHE_SYNC) {
            assert(l2_writes[i].value == 0U);
            sync_writes++;
        } else {
            assert(0);
        }
    }
    assert(clean_writes == len / LINE);
    assert(sync_writes <= batches);
    assert(l2_syncs <= batches);
    assert(l2_debug_writes == 0U);
    assert(lock_holds <= batches);
    assert(max_bytes_per_hold <= L2_MAINT_CHUNK_BYTES);
    assert(l2_ops_unlocked == 0U);
    assert(irq_masked == 0U);
}

/* Flush pushes every dirty byte of the range to DDR, in bounded chunks. */
static void test_flush_reaches_ddr(void)
{
    const u32 buf = 0x1003U;
    const u32 len = 9000U;
    u32 a;

    reset_model();
    for (a = buf; a < buf + len; a++)
        cpu_store(a, (uint8_t)(a * 3U));
    l2_maint_dcache_flush_range((INTPTR)buf, len);
    for (a = buf; a < buf + len; a++)
        assert(ddr[a - BASE] == (uint8_t)(a * 3U));
    assert(l2_clean_inv_calls == (len + (buf & (LINE - 1U)) + LINE - 1U) / LINE);
    assert(l2_inv_calls == 0U);
    assert(l2_syncs == 3U);
    assert(l2_debug_writes == 0U);
    assert(l2_ops_unlocked == 0U);
    assert(max_bytes_per_hold <= L2_MAINT_CHUNK_BYTES);
    assert(lock_holds >= (len + L2_MAINT_CHUNK_BYTES - 1U) / L2_MAINT_CHUNK_BYTES);
    assert(irq_masked == 0U);
}

static void test_whole_cache_flush(void)
{
    reset_model();
    cpu_store(0x7000U, 0x42U);
    test_cpu = 0;
    l2_maint_dcache_flush();
    assert(ddr[0x7000U - BASE] == 0x42U);
    l2_maint_l2_flush();
    assert(l2_ops_unlocked == 0U);
    assert(irq_masked == 0U);
    test_cpu = 1;
}

/* The app loader: copy code on core 1, clean L1 to the unified L2 only,
 * then invalidate the I-side.  The fetched code must be what was written
 * (the BSP's Xil_ICacheInvalidateRange also invalidated L2 and fetched
 * power-on DDR instead). */
static void test_loader_code_visible(void)
{
    u32 a;

    reset_model();
    for (a = 0x8000U; a < 0x8100U; a++)
        cpu_store(a, (uint8_t)(a ^ 0x5AU));
    l2_maint_platform_l1_flush_range((INTPTR)0x8000U, 0x100U);
    l2_maint_icache_invalidate_range((INTPTR)0x8000U, 0x100U);
    for (a = 0x8000U; a < 0x8100U; a++)
        assert(fetch(a) == (uint8_t)(a ^ 0x5AU));
    assert(l1_icache_invalidates == 1U);
    assert(l2_ops_unlocked == 0U);
}

int main(void)
{
    test_cpu = 1;
    test_invalidate_batches_sync();
    test_invalidate_keeps_neighbours();
    test_invalidate_within_one_line();
    test_l2_invalidate_keeps_neighbours();
    test_present_clean_reaches_ddr();
    test_flush_reaches_ddr();
    test_whole_cache_flush();
    test_loader_code_visible();
    puts("l2 maintenance tests passed");
    return 0;
}
