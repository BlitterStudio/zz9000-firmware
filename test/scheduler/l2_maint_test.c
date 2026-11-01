#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "l2_maint.h"
#include "sdk_smp_lock.h"

typedef intptr_t INTPTR;
typedef uint32_t u32;

static int test_cpu;
static uint32_t irq_masked;
static unsigned l1_flush_ranges;
static unsigned l1_invalidate_ranges;
static unsigned l1_flushes;
static unsigned l2_flush_ranges;
static unsigned l2_invalidate_ranges;
static unsigned l2_flushes;
static INTPTR addresses[8];
static u32 lengths[8];

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
}
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word != 0U)
        return 0;
    *word = 1U;
    return 1;
}
void smp_raw_spin_unlock(volatile uint32_t *word)
{
    assert(*word == 1U);
    *word = 0U;
}

void l2_maint_platform_l1_flush_range(INTPTR address, u32 length)
{
    assert(irq_masked == 0U);
    assert(address == (INTPTR)0x1003U);
    assert(length == 9000U);
    l1_flush_ranges++;
}
void l2_maint_platform_l1_invalidate_range(INTPTR address, u32 length)
{
    assert(irq_masked == 0U);
    assert(address == (INTPTR)0x2001U);
    assert(length == 4097U);
    l1_invalidate_ranges++;
}
void l2_maint_platform_l1_flush(void)
{
    assert(irq_masked == 0U);
    l1_flushes++;
}
void l2_maint_platform_l2_flush_range(INTPTR address, u32 length)
{
    assert(irq_masked == 1U);
    addresses[l2_flush_ranges] = address;
    lengths[l2_flush_ranges++] = length;
}
void l2_maint_platform_l2_invalidate_range(INTPTR address, u32 length)
{
    assert(irq_masked == 1U);
    addresses[l2_invalidate_ranges] = address;
    lengths[l2_invalidate_ranges++] = length;
}
void l2_maint_platform_l2_flush(void)
{
    assert(irq_masked == 1U);
    l2_flushes++;
}

int main(void)
{
    test_cpu = 1;
    l2_maint_dcache_flush_range((INTPTR)0x1003U, 9000U);
    assert(l1_flush_ranges == 1U);
    assert(l2_flush_ranges == 3U);
    assert(addresses[0] == (INTPTR)0x1003U && lengths[0] == 4096U);
    assert(addresses[1] == (INTPTR)0x2003U && lengths[1] == 4096U);
    assert(addresses[2] == (INTPTR)0x3003U && lengths[2] == 808U);
    assert(irq_masked == 0U);

    l2_maint_dcache_invalidate_range((INTPTR)0x2001U, 4097U);
    assert(l1_invalidate_ranges == 1U);
    assert(l2_invalidate_ranges == 2U);
    assert(addresses[0] == (INTPTR)0x2001U && lengths[0] == 4096U);
    assert(addresses[1] == (INTPTR)0x3001U && lengths[1] == 1U);
    assert(irq_masked == 0U);

    test_cpu = 0;
    l2_maint_dcache_flush();
    l2_maint_l2_flush();
    assert(l1_flushes == 1U);
    assert(l2_flushes == 2U);
    assert(irq_masked == 0U);

    puts("l2 maintenance tests passed");
    return 0;
}
