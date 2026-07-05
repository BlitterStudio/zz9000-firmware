/* sdk_smp_lock.h - recursive, IRQ-safe, cross-core allocator lock */
#ifndef SDK_SMP_LOCK_H
#define SDK_SMP_LOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t raw;          /* raw spinlock word: 0 free, 1 held */
    volatile int32_t  owner;        /* CPU id of holder, -1 when free     */
    volatile uint32_t depth;        /* recursion count                    */
    volatile uint32_t saved_irq;    /* IRQ state captured at depth 0->1   */
} sdk_smp_lock_t;

#define SDK_SMP_LOCK_INIT { 0u, -1, 0u, 0u }

/* Platform primitives (ARM in sdk_smp_lock_arm.c; mocked in host tests). */
int      smp_cpu_id(void);
uint32_t smp_local_irq_save(void);         /* disable local IRQs, return prior state */
void     smp_local_irq_restore(uint32_t s);
void     smp_raw_spin_lock(volatile uint32_t *word);
void     smp_raw_spin_unlock(volatile uint32_t *word);

void sdk_smp_lock_acquire(sdk_smp_lock_t *l);
void sdk_smp_lock_release(sdk_smp_lock_t *l);

/* Force a lock to the free state, unconditionally. Portable (host-testable):
 * touches only the sdk_smp_lock_t fields, no platform calls. For fault
 * recovery when the owning core cannot run its normal release path (e.g. it
 * was reset mid-critical-section) -- NOT a substitute for sdk_smp_lock_release
 * in ordinary code paths. */
void sdk_smp_lock_reset(sdk_smp_lock_t *l);

/* ARM-only wrappers (implemented in sdk_smp_lock_arm.c under
 * #ifndef SMP_LOCK_HOST_TEST) operating on the file-static cross-core
 * allocator lock g_malloc_lock. Used by core-1 fault recovery (core2.c) to
 * free a lock that core 1 could have orphaned by faulting/being reset while
 * holding it mid-malloc/free. */
void sdk_smp_lock_reset_malloc(void);          /* unconditional reset + dsb/sev */
void sdk_smp_lock_reset_malloc_if_owned(void); /* reset + dsb/sev only if this core owns it */

#endif /* SDK_SMP_LOCK_H */
