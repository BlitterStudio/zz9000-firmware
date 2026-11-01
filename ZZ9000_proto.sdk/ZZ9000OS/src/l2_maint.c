#include "l2_maint.h"
#include "sdk_smp_lock.h"

#ifdef L2_MAINT_HOST_TEST
typedef intptr_t INTPTR;
typedef uint32_t u32;
#else
#include <stdio.h>
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

#define L2_MAINT_LINE_BYTES 32U

#ifdef L2_MAINT_HOST_TEST
extern void l2_maint_platform_l1_flush_range(INTPTR address, u32 length);
extern void l2_maint_platform_l1_flush(void);
extern void l2_maint_platform_l1_clean_inv_line(INTPTR address);
extern void l2_maint_platform_l1_inv_line(INTPTR address);
extern void l2_maint_platform_l2_flush_range(INTPTR address, u32 length);
extern void l2_maint_platform_l2_clean_inv_line(INTPTR address);
extern void l2_maint_platform_l2_inv_line(INTPTR address);
extern void l2_maint_platform_l2_sync(void);
extern void l2_maint_platform_l2_flush(void);
extern void l2_maint_platform_l1_icache_invalidate_range(INTPTR address, u32 length);
#else
static void l2_maint_platform_l1_flush_range(INTPTR address, u32 length)
{
    Xil_L1DCacheFlushRange((u32)address, length);
}

static void l2_maint_platform_l1_flush(void)
{
    Xil_L1DCacheFlush();
}

static void l2_maint_platform_l1_clean_inv_line(INTPTR address)
{
    Xil_L1DCacheFlushLine((u32)address);
}

static void l2_maint_platform_l1_inv_line(INTPTR address)
{
    Xil_L1DCacheInvalidateLine((u32)address);
}

static void l2_maint_platform_l2_flush_range(INTPTR address, u32 length)
{
    Xil_L2CacheFlushRange((u32)address, length);
}

/* Same sequence as the BSP's Xil_DCacheInvalidateRange edge handling,
 * including the PL310 errata 588369/727915 debug-control bracket. */
static void l2_maint_platform_l2_clean_inv_line(INTPTR address)
{
#if defined(CONFIG_PL310_ERRATA_588369) || defined(CONFIG_PL310_ERRATA_727915)
    Xil_Out32(XPS_L2CC_BASEADDR + XPS_L2CC_DEBUG_CTRL_OFFSET, 0x3U);
#endif
    Xil_L2CacheFlushLine((u32)address);
#if defined(CONFIG_PL310_ERRATA_588369) || defined(CONFIG_PL310_ERRATA_727915)
    Xil_Out32(XPS_L2CC_BASEADDR + XPS_L2CC_DEBUG_CTRL_OFFSET, 0x0U);
#endif
}

static void l2_maint_platform_l2_inv_line(INTPTR address)
{
    Xil_Out32(XPS_L2CC_BASEADDR + XPS_L2CC_CACHE_INVLD_PA_OFFSET, (u32)address);
}

static void l2_maint_platform_l2_sync(void)
{
    Xil_Out32(XPS_L2CC_BASEADDR + XPS_L2CC_CACHE_SYNC_OFFSET, 0x0U);
    dsb();
}

/* The BSP's own way flush: l2_maint.o is linked with --wrap like every other
 * object, so the unwrapped symbol has to be named explicitly. */
extern void __real_Xil_L2CacheFlush(void);

static void l2_maint_platform_l2_flush(void)
{
    __real_Xil_L2CacheFlush();
}

static void l2_maint_platform_l1_icache_invalidate_range(INTPTR address, u32 length)
{
    Xil_L1ICacheInvalidateRange((u32)address, length);
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

/*
 * Invalidate L1 and L2 for [address, address + length).  Lines only partly
 * inside the range are cleaned and invalidated first, exactly as the BSP's
 * Xil_DCacheInvalidateRange does: plain invalidation of a partial line would
 * discard dirty data that merely shares the line with the buffer (a static
 * next to a DMA buffer then reverts to whatever DDR held).  Whole lines are
 * invalidated L2 first, then L1, per line, so an L1 refill cannot pick up a
 * stale L2 copy.  At most L2_MAINT_CHUNK_BYTES are processed per lock hold.
 */
static void l2_maint_invalidate_lines(INTPTR address, u32 length, int with_l1)
{
    INTPTR start = address;
    INTPTR end = address + (INTPTR)length;
    INTPTR line_mask = (INTPTR)(L2_MAINT_LINE_BYTES - 1U);

    if ((start & line_mask) != 0) {
        start &= ~line_mask;
        l2_maint_acquire();
        if (with_l1)
            l2_maint_platform_l1_clean_inv_line(start);
        l2_maint_platform_l2_clean_inv_line(start);
        l2_maint_platform_l2_sync();
        l2_maint_release();
        start += (INTPTR)L2_MAINT_LINE_BYTES;
    }
    if ((end & line_mask) != 0) {
        INTPTR last = end & ~line_mask;

        l2_maint_acquire();
        if (with_l1)
            l2_maint_platform_l1_clean_inv_line(last);
        l2_maint_platform_l2_clean_inv_line(last);
        l2_maint_platform_l2_sync();
        l2_maint_release();
        end = last;
    }
    while (start < end) {
        INTPTR chunk_end = start + (INTPTR)L2_MAINT_CHUNK_BYTES;

        if (chunk_end > end)
            chunk_end = end;
        l2_maint_acquire();
        while (start < chunk_end) {
            l2_maint_platform_l2_inv_line(start);
            l2_maint_platform_l2_sync();
            if (with_l1)
                l2_maint_platform_l1_inv_line(start);
            start += (INTPTR)L2_MAINT_LINE_BYTES;
        }
        l2_maint_release();
    }
}

void l2_maint_dcache_invalidate_range(INTPTR address, u32 length)
{
    if (length == 0U)
        return;
    l2_maint_invalidate_lines(address, length, 1);
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
    if (length == 0U)
        return;
    l2_maint_invalidate_lines(address, length, 0);
}

void l2_maint_icache_invalidate_range(INTPTR address, u32 length)
{
    if (length == 0U)
        return;
    l2_maint_platform_l1_icache_invalidate_range(address, length);
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

#ifndef L2_MAINT_HOST_TEST
void l2_maint_report(const char *tag)
{
    printf("[l2] %s: lock owner=%ld depth=%lu core0_waiting=%lu way_op=%04lx "
           "debug=%lx ris=%03lx\r\n", tag,
           (long)l2_maint_lock.owner, (unsigned long)l2_maint_lock.depth,
           (unsigned long)l2_core0_waiting,
           (unsigned long)(Xil_In32(XPS_L2CC_BASEADDR +
                                    XPS_L2CC_CACHE_INV_CLN_WAY_OFFSET) & 0xffffU),
           (unsigned long)Xil_In32(XPS_L2CC_BASEADDR + XPS_L2CC_DEBUG_CTRL_OFFSET),
           (unsigned long)(Xil_In32(XPS_L2CC_BASEADDR + XPS_L2CC_ISR_OFFSET) & 0x1ffU));
}

/* Link-time targets of -Wl,--wrap=<symbol> (Makefile). */
void __wrap_Xil_DCacheFlushRange(INTPTR address, u32 length)
{
    l2_maint_dcache_flush_range(address, length);
}

void __wrap_Xil_DCacheInvalidateRange(INTPTR address, u32 length)
{
    l2_maint_dcache_invalidate_range(address, length);
}

void __wrap_Xil_DCacheFlush(void)
{
    l2_maint_dcache_flush();
}

void __wrap_Xil_L2CacheFlush(void)
{
    l2_maint_l2_flush();
}

void __wrap_Xil_ICacheInvalidateRange(INTPTR address, u32 length)
{
    l2_maint_icache_invalidate_range(address, length);
}
#endif
