#define L2_MAINT_NO_REDIRECT
#include "l2_maint.h"
#include "sdk_smp_lock.h"
#ifdef Xil_DCacheFlushRange
#undef Xil_DCacheFlushRange
#undef Xil_DCacheInvalidateRange
#undef Xil_DCacheFlush
#undef Xil_L2CacheFlush
#endif


#ifdef L2_MAINT_HOST_TEST
typedef intptr_t INTPTR;
typedef uint32_t u32;
#else
#include "xil_types.h"
#include "xil_cache_l.h"
#include "xil_io.h"
#include "xl2cc.h"
#include "xpseudo_asm.h"
#endif

static sdk_smp_lock_t l2_maint_lock = SDK_SMP_LOCK_INIT;
static volatile uint32_t l2_core0_waiting;

static void l2_maint_publish(void)
{
#ifdef L2_MAINT_HOST_TEST
    __sync_synchronize();
#else
    __asm__ __volatile__("dmb ish" ::: "memory");
#endif
}

/* Core 0 advertises before acquiring. Core 1 never starts another chunk once
 * that advertisement is visible, so a vblank waiter sees at most the active
 * 4 KiB core-1 chunk. */
static void l2_maint_acquire(void)
{
    if (smp_cpu_id() == 0) {
        if (l2_maint_lock.owner != 0) {
            l2_core0_waiting = 1U;
            l2_maint_publish();
        }
        sdk_smp_lock_acquire(&l2_maint_lock);
        return;
    }

    for (;;) {
        while (l2_core0_waiting != 0U)
            ;
        if (!sdk_smp_lock_try_acquire(&l2_maint_lock))
            continue;
        if (l2_core0_waiting == 0U)
            return;
        sdk_smp_lock_release(&l2_maint_lock);
    }
}

static void l2_maint_release(void)
{
    if (smp_cpu_id() == 0 && l2_maint_lock.depth == 1U) {
        l2_core0_waiting = 0U;
        l2_maint_publish();
    }
    sdk_smp_lock_release(&l2_maint_lock);
}

#ifdef L2_MAINT_HOST_TEST
extern void l2_maint_platform_l1_flush_range(INTPTR address, u32 length);
extern void l2_maint_platform_l1_invalidate_range(INTPTR address, u32 length);
extern void l2_maint_platform_l1_flush(void);
extern void l2_maint_platform_l2_flush_range(INTPTR address, u32 length);
extern void l2_maint_platform_l2_invalidate_range(INTPTR address, u32 length);
extern void l2_maint_platform_l2_flush(void);
#else
static void l2_maint_platform_l1_flush_range(INTPTR address, u32 length)
{
    Xil_L1DCacheFlushRange((u32)address, length);
}

static void l2_maint_platform_l1_invalidate_range(INTPTR address, u32 length)
{
    Xil_L1DCacheInvalidateRange((u32)address, length);
}

static void l2_maint_platform_l1_flush(void)
{
    Xil_L1DCacheFlush();
}

static void l2_maint_platform_l2_flush_range(INTPTR address, u32 length)
{
    Xil_L2CacheFlushRange((u32)address, length);
}

static void l2_maint_platform_l2_invalidate_range(INTPTR address, u32 length)
{
    Xil_L2CacheInvalidateRange((u32)address, length);
}

static void l2_maint_platform_l2_flush(void)
{
    Xil_L2CacheFlush();
}
#endif

typedef void (*l2_maint_range_op_t)(INTPTR address, u32 length);

static void l2_maint_range(INTPTR address, u32 length, l2_maint_range_op_t op)
{
    while (length != 0U) {
        u32 chunk = length > L2_MAINT_CHUNK_BYTES ?
            L2_MAINT_CHUNK_BYTES : length;

        l2_maint_acquire();
        op(address, chunk);
        l2_maint_release();
        address += chunk;
        length -= chunk;
    }
}

void l2_maint_dcache_flush_range(INTPTR address, u32 length)
{
    if (length == 0U)
        return;
    /* L1 is private to the executing core; it must not extend the PL310 lock. */
    l2_maint_platform_l1_flush_range(address, length);
    l2_maint_range(address, length, l2_maint_platform_l2_flush_range);
}

void l2_maint_dcache_invalidate_range(INTPTR address, u32 length)
{
    if (length == 0U)
        return;
    /* This preserves BSP partial-line handling while serializing PL310 writes. */
    l2_maint_platform_l1_invalidate_range(address, length);
    l2_maint_range(address, length, l2_maint_platform_l2_invalidate_range);
}

void l2_maint_dcache_flush(void)
{
    l2_maint_platform_l1_flush();
    l2_maint_acquire();
    l2_maint_platform_l2_flush();
    l2_maint_release();
}

void l2_maint_l2_flush(void)
{
    l2_maint_acquire();
    l2_maint_platform_l2_flush();
    l2_maint_release();
}

void l2_maint_l2_invalidate_range(INTPTR address, u32 length)
{
#ifdef L2_MAINT_HOST_TEST
    if (length != 0U)
        l2_maint_range(address, length, l2_maint_platform_l2_invalidate_range);
#else
    INTPTR end;

    if (length == 0U)
        return;
    end = address + length;
    address &= ~(INTPTR)31U;
    while (address < end) {
        INTPTR chunk_end = address + L2_MAINT_CHUNK_BYTES;
        volatile u32 *invalidate = (volatile u32 *)(XPS_L2CC_BASEADDR +
            XPS_L2CC_CACHE_INVLD_PA_OFFSET);
        volatile u32 *sync = (volatile u32 *)(XPS_L2CC_BASEADDR +
            XPS_L2CC_CACHE_SYNC_OFFSET);

        if (chunk_end > end)
            chunk_end = end;
        l2_maint_acquire();
        while (address < chunk_end) {
            *invalidate = (u32)address;
            while ((*invalidate & 1U) != 0U)
                ;
            address += 32U;
        }
        *sync = 0U;
        dsb();
        l2_maint_release();
    }
#endif
}

void l2_maint_reset_core1_lock(void)
{
    if (l2_maint_lock.owner == 1) {
        sdk_smp_lock_reset(&l2_maint_lock);
        l2_core0_waiting = 0U;
#ifndef L2_MAINT_HOST_TEST
        __asm__ __volatile__("dsb\n\tsev" ::: "memory");
#endif
    }
}
