#include "sdk_smp_lock.h"

void sdk_smp_lock_acquire(sdk_smp_lock_t *l)
{
    uint32_t flags = smp_local_irq_save();   /* mask local IRQs first */
    int me = smp_cpu_id();
    if (l->owner == me) {
        l->depth++;
        /* We were already the owner => IRQs were already masked at depth 0.
         * Undo this call's extra mask to keep save/restore balanced. */
        smp_local_irq_restore(flags);
        return;
    }
    smp_raw_spin_lock(&l->raw);               /* spin with local IRQs masked */
    l->owner = me;
    l->depth = 1u;
    l->saved_irq = flags;                     /* original state, restored at depth 0 */
    /* keep IRQs masked until release at depth 0 */
}

void sdk_smp_lock_release(sdk_smp_lock_t *l)
{
    /* Precondition: caller is the owner and local IRQs are masked. */
    if (l->depth == 0u) return;               /* defensive: unbalanced release */
    l->depth--;
    if (l->depth == 0u) {
        uint32_t flags = l->saved_irq;
        l->owner = -1;
        smp_raw_spin_unlock(&l->raw);
        smp_local_irq_restore(flags);
    }
}
