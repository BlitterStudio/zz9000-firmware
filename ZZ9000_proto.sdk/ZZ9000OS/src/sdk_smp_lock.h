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

#endif /* SDK_SMP_LOCK_H */
