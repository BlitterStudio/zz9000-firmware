/* Host test: mock the platform primitives, exercise the state machine. */
#include "sdk_smp_lock.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>

/* Per-thread CPU id: real cores each have a fixed, hardware-distinct id, so
 * smp_cpu_id() can never collide across cores. A single shared global here
 * would let a second thread's unsynchronized "owner == me" read alias onto
 * the first thread's id mid-critical-section, letting it take the recursive
 * fast path without ever taking the raw spinlock. Thread-local storage
 * models "one fixed id per core" faithfully instead. */
static __thread int t_cpu = 0;          /* test-controlled current CPU */
static int   g_irq_disabled_depth = 0;  /* tracks nested disables      */
static pthread_mutex_t g_raw = PTHREAD_MUTEX_INITIALIZER;

int      smp_cpu_id(void) { return t_cpu; }
uint32_t smp_local_irq_save(void) { int was = g_irq_disabled_depth; g_irq_disabled_depth++; return (uint32_t)was; }
void     smp_local_irq_restore(uint32_t s) { g_irq_disabled_depth = (int)s; }
void     smp_raw_spin_lock(volatile uint32_t *w) { pthread_mutex_lock(&g_raw); *w = 1u; }
void     smp_raw_spin_unlock(volatile uint32_t *w) { *w = 0u; pthread_mutex_unlock(&g_raw); }

static void test_recursion_balances_irq(void) {
    sdk_smp_lock_t l = SDK_SMP_LOCK_INIT;
    t_cpu = 0; g_irq_disabled_depth = 0;
    sdk_smp_lock_acquire(&l);            /* depth 0->1 */
    assert(l.owner == 0 && l.depth == 1);
    sdk_smp_lock_acquire(&l);            /* recursive */
    assert(l.depth == 2);
    sdk_smp_lock_release(&l);
    assert(l.depth == 1 && l.owner == 0);
    assert(g_irq_disabled_depth > 0);    /* still masked while held */
    sdk_smp_lock_release(&l);
    assert(l.owner == -1 && l.depth == 0);
    assert(g_irq_disabled_depth == 0);   /* restored exactly at depth 0 */
    printf("test_recursion_balances_irq OK\n");
}

/* Fault-recovery reset: must force a HELD lock back to the free state, and
 * the freed lock must be cleanly re-acquirable afterwards (models core-1
 * restart finding g_malloc_lock stranded mid-malloc/free and releasing it). */
static void test_reset_frees_held_lock(void) {
    sdk_smp_lock_t l = SDK_SMP_LOCK_INIT;
    t_cpu = 0; g_irq_disabled_depth = 0;
    sdk_smp_lock_acquire(&l);            /* held: owner=0, depth=1 */
    assert(l.owner == 0 && l.depth == 1);

    sdk_smp_lock_reset(&l);
    assert(l.owner == -1 && l.depth == 0 && l.raw == 0);
    /* Test-mock-only bookkeeping: the raw word here is backed by a real
     * pthread_mutex (g_raw) so test_stress_no_lost_updates gets genuine
     * cross-thread exclusion. sdk_smp_lock_reset only touches the
     * sdk_smp_lock_t fields (matching production, where raw is a bare
     * spinlock word and writing 0 to it IS the unlock) -- so free the mock's
     * backing mutex here too, or the acquire below deadlocks on it. */
    pthread_mutex_unlock(&g_raw);

    /* freshly acquirable again after reset */
    sdk_smp_lock_acquire(&l);
    assert(l.owner == 0 && l.depth == 1);
    sdk_smp_lock_release(&l);
    assert(l.owner == -1 && l.depth == 0);
    printf("test_reset_frees_held_lock OK\n");
}

/* 2-thread stress: increments under the lock must never race. */
static sdk_smp_lock_t g_stress = SDK_SMP_LOCK_INIT;
static long g_counter = 0;
static void *worker(void *arg) {
    int id = (int)(long)arg;
    t_cpu = id;   /* this worker models one fixed, distinct CPU core */
    for (int i = 0; i < 100000; i++) {
        sdk_smp_lock_acquire(&g_stress);
        long v = g_counter; v++; g_counter = v;
        sdk_smp_lock_release(&g_stress);
    }
    return NULL;
}
static void test_stress_no_lost_updates(void) {
    pthread_t a, b;
    g_counter = 0;
    pthread_create(&a, NULL, worker, (void*)0L);
    pthread_create(&b, NULL, worker, (void*)1L);
    pthread_join(a, NULL); pthread_join(b, NULL);
    assert(g_counter == 200000);
    printf("test_stress_no_lost_updates OK\n");
}

int main(void) {
    test_recursion_balances_irq();
    test_reset_frees_held_lock();
    test_stress_no_lost_updates();
    printf("ALL smp_lock tests passed\n");
    return 0;
}
