/* Shared PL310 maintenance serialization. */
#ifndef L2_MAINT_H
#define L2_MAINT_H

#include <stdint.h>

/*
 * PL310 r3p3 TRM, "Background operations" (3.3.11): no maintenance
 * register may be written while a way operation is in progress.  All firmware
 * L2 maintenance therefore goes through these functions.  Core 1 holds the
 * lock for at most L2_MAINT_CHUNK_BYTES; a waiting core 0 takes priority at
 * the next chunk, so vblank can wait for no more than one 4 KiB chunk.
 */
#define L2_MAINT_CHUNK_BYTES 4096U

void l2_maint_dcache_flush_range(intptr_t address, uint32_t length);
void l2_maint_dcache_invalidate_range(intptr_t address, uint32_t length);
void l2_maint_dcache_flush(void);
void l2_maint_l2_flush(void);

/* L2-only invalidation for aligned DMA-owned lines. */
void l2_maint_l2_invalidate_range(intptr_t address, uint32_t length);

/* Call only after core 1 is known halted during a cold restart. */
void l2_maint_reset_core1_lock(void);

#ifndef L2_MAINT_NO_REDIRECT
/* Keep BSP callers from bypassing the shared PL310 lock. */
#define Xil_DCacheFlushRange l2_maint_dcache_flush_range
#define Xil_DCacheInvalidateRange l2_maint_dcache_invalidate_range
#define Xil_DCacheFlush l2_maint_dcache_flush
#define Xil_L2CacheFlush l2_maint_l2_flush
#endif

#endif
