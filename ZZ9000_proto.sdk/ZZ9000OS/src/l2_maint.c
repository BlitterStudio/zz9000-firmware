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
#include "xil_errata.h"
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
#define L2_MAINT_REG_CACHE_SYNC       0x0730U
#define L2_MAINT_REG_CACHE_INVLD_PA   0x0770U
#define L2_MAINT_REG_CACHE_CLEAN_PA   0x07B0U
#define L2_MAINT_REG_CACHE_INV_CLN_PA 0x07F0U
#define L2_MAINT_REG_DEBUG_CTRL       0x0F40U
#else
#define L2_MAINT_REG_CACHE_SYNC       XPS_L2CC_CACHE_SYNC_OFFSET
#define L2_MAINT_REG_CACHE_INVLD_PA   XPS_L2CC_CACHE_INVLD_PA_OFFSET
#define L2_MAINT_REG_CACHE_CLEAN_PA   XPS_L2CC_CACHE_CLEAN_PA_OFFSET
#define L2_MAINT_REG_CACHE_INV_CLN_PA XPS_L2CC_CACHE_INV_CLN_PA_OFFSET
#define L2_MAINT_REG_DEBUG_CTRL       XPS_L2CC_DEBUG_CTRL_OFFSET
#endif


#ifdef L2_MAINT_HOST_TEST
extern void l2_maint_platform_l1_flush_range(INTPTR address, u32 length);
extern void l2_maint_platform_l1_clean_range(INTPTR address, u32 length);
extern void l2_maint_platform_l1_flush(void);
extern void l2_maint_platform_l1_clean_inv_line(INTPTR address);
extern void l2_maint_platform_l1_inv_line(INTPTR address);
extern void l2_maint_platform_l2_write(u32 offset, u32 value);
extern void l2_maint_platform_l2_flush(void);
extern void l2_maint_platform_l1_icache_invalidate_range(INTPTR address, u32 length);
#else
static void l2_maint_platform_l1_flush_range(INTPTR address, u32 length)
{
    Xil_L1DCacheFlushRange((u32)address, length);
}

/* Scanout only needs dirty L1 lines to reach the shared PL310.  Keep the
 * lines valid so the next frame does not incur a write-allocate miss. */
static void l2_maint_platform_l1_clean_range(INTPTR address, u32 length)
{
    u32 line = (u32)address & ~(L2_MAINT_LINE_BYTES - 1U);
    u32 end = (u32)address + length;

    mtcp(XREG_CP15_CACHE_SIZE_SEL, 0U);
    while (line < end) {
        mtcp(XREG_CP15_CLEAN_DC_LINE_MVA_POC, line);
        line += L2_MAINT_LINE_BYTES;
    }
    dsb();
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

static void l2_maint_platform_l2_write(u32 offset, u32 value)
{
    Xil_Out32(XPS_L2CC_BASEADDR + offset, value);
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

static void l2_maint_platform_l2_clean_line(INTPTR address)
{
    l2_maint_platform_l2_write(L2_MAINT_REG_CACHE_CLEAN_PA, (u32)address);
}

/* Zynq's PL310 r3p2 is not affected by 588369, so its atomic combined
 * operation is correct.  Erratum 727915 only concerns by-way operations. */
static void l2_maint_platform_l2_clean_inv_line(INTPTR address)
{
    l2_maint_platform_l2_write(L2_MAINT_REG_CACHE_INV_CLN_PA, (u32)address);
}

static void l2_maint_platform_l2_inv_line(INTPTR address)
{
    l2_maint_platform_l2_write(L2_MAINT_REG_CACHE_INVLD_PA, (u32)address);
}

/* PL310 r3p2 PA maintenance operations are atomic. Cache Sync closes each
 * serialized batch before its L1 invalidations or unlock. */
static void l2_maint_platform_l2_sync(void)
{
    l2_maint_platform_l2_write(L2_MAINT_REG_CACHE_SYNC, 0x0U);
#ifndef L2_MAINT_HOST_TEST
    dsb();
#endif
}


static void l2_maint_clean_inv_lines(INTPTR address, u32 length)
{
    INTPTR start = address & ~(INTPTR)(L2_MAINT_LINE_BYTES - 1U);
    INTPTR end = address + (INTPTR)length;

    while (start < end) {
        INTPTR chunk_end = start + (INTPTR)L2_MAINT_CHUNK_BYTES;

        if (chunk_end > end)
            chunk_end = end;
        l2_maint_acquire();
        while (start < chunk_end) {
            l2_maint_platform_l2_clean_inv_line(start);
            start += (INTPTR)L2_MAINT_LINE_BYTES;
        }
        l2_maint_platform_l2_sync();
        l2_maint_release();
    }
}

/* A scanout reader on the HP port observes DDR, not the PL310.  Clean the
 * surface without invalidating it; the r3p2 PA clean operation needs neither
 * of the BSP's legacy 588369/727915 debug-control workarounds. */
static void l2_maint_clean_lines(INTPTR address, u32 length)
{
    INTPTR start = address & ~(INTPTR)(L2_MAINT_LINE_BYTES - 1U);
    INTPTR end = address + (INTPTR)length;

    while (start < end) {
        INTPTR chunk_end = start + (INTPTR)L2_MAINT_CHUNK_BYTES;

        if (chunk_end > end)
            chunk_end = end;
        l2_maint_acquire();
        while (start < chunk_end) {
            l2_maint_platform_l2_clean_line(start);
            start += (INTPTR)L2_MAINT_LINE_BYTES;
        }
        l2_maint_platform_l2_sync();
        l2_maint_release();
    }
}

void l2_maint_dcache_clean_range(INTPTR address, u32 length)
{
    if (length == 0U)
        return;
    /* L1 is private to the executing core; it must not extend the PL310 lock. */
    l2_maint_platform_l1_clean_range(address, length);
    l2_maint_clean_lines(address, length);
}

void l2_maint_dcache_flush_range(INTPTR address, u32 length)
{
    if (length == 0U)
        return;
    /* L1 is private to the executing core; it must not extend the PL310 lock. */
    l2_maint_platform_l1_flush_range(address, length);
    l2_maint_clean_inv_lines(address, length);
}

/*
 * Invalidate L1 and L2 for [address, address + length).  Lines only partly
 * inside the range are cleaned and invalidated first, exactly as the BSP's
 * Xil_DCacheInvalidateRange does: plain invalidation of a partial line would
 * discard dirty data that merely shares the line with the buffer (a static
 * next to a DMA buffer then reverts to whatever DDR held).  Whole lines are
 * invalidated in L2, synchronized, then invalidated in L1, so an L1 refill
 * cannot pick up a stale L2 copy.  At most L2_MAINT_CHUNK_BYTES are processed
 * per lock hold.
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

        if (last >= start) {
            l2_maint_acquire();
            if (with_l1)
                l2_maint_platform_l1_clean_inv_line(last);
            l2_maint_platform_l2_clean_inv_line(last);
            l2_maint_platform_l2_sync();
            l2_maint_release();
        }
        end = last;
    }
    while (start < end) {
        INTPTR chunk_end = start + (INTPTR)L2_MAINT_CHUNK_BYTES;
        INTPTR line;

        if (chunk_end > end)
            chunk_end = end;
        l2_maint_acquire();
        for (line = start; line < chunk_end;
             line += (INTPTR)L2_MAINT_LINE_BYTES)
            l2_maint_platform_l2_inv_line(line);
        l2_maint_platform_l2_sync();
        if (with_l1) {
            for (line = start; line < chunk_end;
                 line += (INTPTR)L2_MAINT_LINE_BYTES)
                l2_maint_platform_l1_inv_line(line);
        }
        l2_maint_release();
        start = chunk_end;
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
