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
#define LINES 1024U
#define BYTES (LINES * LINE)

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
}
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word != 0U)
        return 0;
    *word = 1U;
    lock_holds++;
    bytes_this_hold = 0U;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word)
{
    assert(*word == 1U);
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

void l2_maint_platform_l1_flush_range(INTPTR address, u32 length)
{
    u32 a = (u32)address & ~(LINE - 1U);
    u32 end = (u32)address + length;

    for (; a < end; a += LINE)
        l1_clean_inv(index_of((INTPTR)a));
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
void l2_maint_platform_l2_flush_range(INTPTR address, u32 length)
{
    u32 a = (u32)address & ~(LINE - 1U);
    u32 end = (u32)address + length;

    for (; a < end; a += LINE) {
        l2_op_locked();
        l2_clean_inv(index_of((INTPTR)a));
    }
}
void l2_maint_platform_l2_clean_inv_line(INTPTR address)
{
    l2_op_locked();
    l2_clean_inv(index_of(address));
}
void l2_maint_platform_l2_inv_line(INTPTR address)
{
    unsigned i = index_of(address);

    l2_op_locked();
    l2[i].valid = 0;
    l2[i].dirty = 0;
}
void l2_maint_platform_l2_sync(void)
{
    if (irq_masked == 0U)
        l2_ops_unlocked++;
}
void l2_maint_platform_l2_flush(void)
{
    unsigned i;

    if (irq_masked == 0U)
        l2_ops_unlocked++;
    for (i = 0U; i < LINES; i++)
        l2_clean_inv(i);
}

static void reset_model(void)
{
    memset(ddr, 0, sizeof(ddr));
    memset(l1, 0, sizeof(l1));
    memset(l2, 0, sizeof(l2));
    l2_ops_unlocked = 0U;
    lock_holds = 0U;
    max_bytes_per_hold = 0U;
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
    assert(l2_ops_unlocked == 0U);
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
    assert(l2_ops_unlocked == 0U);
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

int main(void)
{
    test_cpu = 1;
    test_invalidate_keeps_neighbours();
    test_invalidate_within_one_line();
    test_l2_invalidate_keeps_neighbours();
    test_flush_reaches_ddr();
    test_whole_cache_flush();
    puts("l2 maintenance tests passed");
    return 0;
}
