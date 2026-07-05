/* sdk_smp_lock_arm.c - Cortex-A9 primitives; compiled only for ARM. */
#ifndef SMP_LOCK_HOST_TEST
#include "sdk_smp_lock.h"
#include "xpseudo_asm.h"
#include "xreg_cortexa9.h"
#include <reent.h>

int smp_cpu_id(void)
{
    uint32_t mpidr;
    __asm__ __volatile__("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return (int)(mpidr & 0xFFu);
}

uint32_t smp_local_irq_save(void)
{
    uint32_t cpsr;
    __asm__ __volatile__("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ __volatile__("cpsid i" ::: "memory");
    return cpsr & (1u << 7);              /* prior I-bit only */
}

void smp_local_irq_restore(uint32_t saved_i_bit)
{
    if (saved_i_bit == 0u) {              /* IRQs were enabled before */
        __asm__ __volatile__("cpsie i" ::: "memory");
    }
}

void smp_raw_spin_lock(volatile uint32_t *word)
{
    /* NOTE: deviates from the task brief's literal asm, which used the same
     * register for both the strex status and the store value
     * ("strex %0, %0, [%1]"). That is rejected by this toolchain's assembler
     * ("registers may not be the same") because STREX requires Rd != Rt.
     * Fixed with a second temporary (value) for the STREX source register;
     * the ldrex/teq/wfene/bne/strex/teq/bne/dmb structure and barrier
     * placement are otherwise transcribed faithfully. */
    uint32_t status, value;
    __asm__ __volatile__(
        "1: ldrex   %0, [%2]        \n"
        "   teq     %0, #0          \n"
        "   wfene                   \n"   /* sleep if held */
        "   bne     1b              \n"
        "   mov     %1, #1          \n"
        "   strex   %0, %1, [%2]    \n"   /* try to take */
        "   teq     %0, #0          \n"
        "   bne     1b              \n"
        "   dmb                     \n"
        : "=&r"(status), "=&r"(value) : "r"(word) : "cc", "memory");
}

void smp_raw_spin_unlock(volatile uint32_t *word)
{
    __asm__ __volatile__(
        "   dmb                     \n"
        "   mov     r1, #0          \n"
        "   str     r1, [%0]        \n"
        "   dsb                     \n"
        "   sev                     \n"
        :: "r"(word) : "r1", "memory");
}

static sdk_smp_lock_t g_malloc_lock = SDK_SMP_LOCK_INIT;

void __malloc_lock(struct _reent *r)   { (void)r; sdk_smp_lock_acquire(&g_malloc_lock); }
void __malloc_unlock(struct _reent *r) { (void)r; sdk_smp_lock_release(&g_malloc_lock); }
#endif /* !SMP_LOCK_HOST_TEST */
