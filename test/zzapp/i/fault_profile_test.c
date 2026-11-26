/*
 * The fault stack snapshot and the profiler of the AAR control block, through
 * the production bodies: a fault, a stall and a damaged canary leave the
 * registers they have and a snapshot of only the app's own stack, and a rate
 * the client asks for becomes a ring, a grant and samples of the pc of the
 * frame a scheduler tick interrupted (zzapp_kernel_arch_dispatch).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aar/app.h"
#include "aar/transport.h"
#include "card_pool.h"
#include "host_card_pool.h"
#include "xil_cache.h"
#include "zzapp_as.h"
#include "zzapp_kernel.h"
#include "zzapp_proto.h"
#include "zzapp_rec.h"
#include "zzapp_runtime.h"
#include "zzapp_state.h"
#include "zzapp_xport.h"

int smp_cpu_id(void) { return 0; }
void smp_raw_spin_lock(volatile uint32_t *word) { *word = 1U; }
int smp_raw_spin_try_lock(volatile uint32_t *word)
{
    if (*word)
        return 0;
    *word = 1U;
    return 1;
}
uint32_t smp_local_irq_save(void) { return 0U; }
void smp_local_irq_restore(uint32_t state) { (void)state; }
void smp_raw_spin_unlock(volatile uint32_t *word) { *word = 0U; }
struct xil_cache_mock_state g_xil_cache_mock;

/* Defined in zzapp_host.c; the loader calls it at load begin. */
void zzapp_loader_bound(uint32_t card, uint8_t *mem, uint32_t owner,
                        uint32_t generation);
zzapp_arm_frame_t *zzapp_kernel_arch_dispatch(zzapp_arm_frame_t *frame,
                                               uint32_t source,
                                               uint32_t return_adjust);

/* ---- A services layer reduced to what the host calls. ------------------- */

static uint32_t selected;

uint32_t zzapp_services_current_pid(void) { return selected; }
int zzapp_services_select_pid(uint32_t pid) { selected = pid; return 1; }
int zzapp_services_take_start(uint32_t *argc)
{
    (void)argc;
    return 0;
}
int zzapp_services_start_entered(uint32_t generation)
{
    (void)generation;
    return AAR_OK;
}
void zzapp_services_on_reset(void) {}
/* Every ended record stays a shell with its transport, as under a client. */
int zzapp_services_retire_wire(struct zzapp_rec *rec)
{
    (void)rec;
    return 0;
}

/* ---- Counting host-visible heap for the transport. ---------------------- */

static unsigned x_allocs;
static unsigned x_frees;
static int fail_ring_alloc;
static void *ring_ptr;
static uint32_t ring_card;

static int count_alloc(void *ctx, uint32_t bytes, uint32_t align, uint32_t lo,
                       uint32_t hi, void **ptr, uint32_t *card)
{
    void *mem = NULL;

    (void)ctx;
    (void)lo;
    (void)hi;
    if (bytes == ZZAPP_PROFILE_RING_BYTES && fail_ring_alloc)
        return -1;
    if (posix_memalign(&mem, align, bytes) != 0)
        return -1;
    *ptr = mem;
    *card = 0x04000000u + ++x_allocs * 0x100000u;
    if (bytes == ZZAPP_PROFILE_RING_BYTES) {
        ring_ptr = mem;
        ring_card = *card;
    }
    return 0;
}

static int count_free(void *ctx, void *ptr, uint32_t card)
{
    (void)ctx;
    (void)card;
    free(ptr);
    x_frees++;
    return 0;
}

static const zzapp_xport_env_t counting_env = {
    .alloc = count_alloc,
    .release = count_free,
    .acquire = zzapp_xport_dmb,
    .release_barrier = zzapp_xport_dmb,
};

/* The app's memory: a window the size of a stack and a little more, at a card
 * address of its own. The stack is [STACK_LO, STACK_HI) of it. */
#define WIN_CARD   0x08000000u
#define WIN_BYTES  0x30000u
#define STACK_LO   0x10000u
#define STACK_HI   0x20000u
#define SECOND_US  1000000ull

static uint8_t window[WIN_BYTES];

static void boot(void)
{
    uint32_t i;

    host_card_pool_reset(256U << 20);
    zzapp_test_set_arena_apps(64U);
    zzapp_boot_init();
    zzapp_xport_init(&counting_env);
    zzapp_test_set_xtime(0U);
    selected = 0U;
    x_allocs = 0U;
    x_frees = 0U;
    fail_ring_alloc = 0;
    ring_ptr = NULL;
    ring_card = 0U;
    memset(window, 0, sizeof(window));
    for (i = STACK_LO; i < STACK_HI; i++)
        window[i] = (uint8_t)(i * 7U + 3U);
}

static uint32_t cb(const zzapp_rec_t *rec, uint32_t off)
{
    return zzapp_xport_cb_get(rec, off);
}

static uint8_t cb_byte(const zzapp_rec_t *rec, uint32_t off)
{
    zzapp_xport_buf_t pair;

    assert(zzapp_xport_block_get(rec, ZZAPP_XPORT_BLOCK_PAIR, &pair));
    return ((const volatile uint8_t *)pair.ptr)[off];
}

/* A claim, its rate word, the load begin and a running app whose stack is
 * the window's. */
static zzapp_rec_t *start_app(uint32_t *pid_out, uint32_t hz)
{
    uint32_t pid = zzapp_test_admit();
    zzapp_rec_t *rec;

    assert(pid != ZZAPP_PID_NONE);
    rec = zzapp_rec_by_pid(pid);
    if (hz != 0U)
        zzapp_xport_cb_put(rec, AAR_CB_PROFILE_HZ, hz);
    zzapp_loader_bound(WIN_CARD, window, 1U, 1U);
    rec->proc.stack_base = STACK_LO;
    rec->proc.stack_top = STACK_HI;
    assert(zzapp_kernel_start_app(zzapp_runtime_kernel(), pid) == 0);
    zzapp_test_set_xtime(0U);
    zzapp_test_mark_running();
    if (pid_out)
        *pid_out = pid;
    return rec;
}

/* ---- The fault record and the snapshot ----------------------------------- */

static void assert_snapshot(const zzapp_rec_t *rec, uint32_t sp,
                            uint32_t bytes)
{
    uint32_t i;

    assert(cb(rec, AAR_CB_STACK_BYTES) == bytes);
    if (bytes == 0U) {
        assert(cb(rec, AAR_CB_STACK_SP_HI) == 0U &&
               cb(rec, AAR_CB_STACK_SP_LO) == 0U);
        return;
    }
    assert(cb(rec, AAR_CB_STACK_SP_HI) == 0U);
    assert(cb(rec, AAR_CB_STACK_SP_LO) == sp);
    for (i = 0U; i < bytes; i++)
        assert(cb_byte(rec, AAR_CB_STACK_DATA + i) ==
               window[sp - WIN_CARD + i]);
}

static void check_fault_snapshot(uint32_t sp, uint32_t expected)
{
    uint32_t pid;
    zzapp_rec_t *rec;

    rec = start_app(&pid, 0U);
    zzapp_take_app_fault(ZZAPP_FAULT_DATA, 0x9000u, sp, 0x9100u, 0xdead0000u,
                         0x805u, 0x60000010u);
    zzapp_worker_hook();
    assert(cb(rec, AAR_CB_STATE) == AAR_STATE_FAULTED);
    assert(cb(rec, AAR_CB_END_REASON) == AAR_END_USER_FAULT);
    assert(cb(rec, AAR_CB_FAULT_CLASS) == AAR_FAULT_DATA);
    assert(cb(rec, AAR_CB_FAULT_PC_LO) == 0x9000u);
    assert(cb(rec, AAR_CB_FAULT_SP_LO) == sp);
    assert(cb(rec, AAR_CB_FAULT_LR_LO) == 0x9100u);
    assert_snapshot(rec, sp, expected);
}

static void test_fault_snapshot_is_bounded_to_the_stack(void)
{
    boot();
    /* 256 bytes to the top of the stack: all of them. */
    check_fault_snapshot(WIN_CARD + STACK_HI - 0x100u, 0x100u);
    /* Far from the top: the 512-byte maximum. */
    check_fault_snapshot(WIN_CARD + STACK_LO + 0x1000u, AAR_CB_STACK_MAX);
    check_fault_snapshot(WIN_CARD + STACK_HI - 4u, 4u);
    /* An sp that is not in the stack reads nothing, whatever it holds. */
    check_fault_snapshot(WIN_CARD + STACK_LO - 4u, 0u);
    check_fault_snapshot(WIN_CARD + STACK_HI, 0u);
    check_fault_snapshot(WIN_CARD + WIN_BYTES, 0u);
    check_fault_snapshot(WIN_CARD - 0x1000u, 0u);
    check_fault_snapshot(0u, 0u);
}

/* The stall watchdog decides in the timer handler; the same tick's frame
 * supplies pc, sp and lr, and the record has class NONE. */
static void test_stall_records_where_the_watchdog_found_the_app(void)
{
    uint32_t pid;
    zzapp_rec_t *rec;
    zzapp_arm_frame_t frame;
    uint32_t sp = WIN_CARD + STACK_HI - 0x80u;

    boot();
    rec = start_app(&pid, 0U);
    zzapp_kernel_set_current_thread((int)pid);
    zzapp_host_tick_at(31u * SECOND_US);
    memset(&frame, 0, sizeof(frame));
    frame.spsr = ZZAPP_KERNEL_APP_CPSR;
    frame.exception_lr = 0x9004u;
    frame.user_sp = sp;
    frame.user_lr = 0x9100u;
    (void)zzapp_kernel_arch_dispatch(&frame, ZZAPP_KERNEL_DISPATCH_TICK, 4u);
    zzapp_worker_hook();
    assert(cb(rec, AAR_CB_END_REASON) == AAR_END_STALL);
    assert(cb(rec, AAR_CB_STATE) == AAR_STATE_FAULTED);
    assert(cb(rec, AAR_CB_FAULT_CLASS) == AAR_FAULT_NONE);
    assert(cb(rec, AAR_CB_FAULT_SYNDROME) == 0U);
    assert(cb(rec, AAR_CB_FAULT_PSTATE) == 0U);
    assert(cb(rec, AAR_CB_FAULT_ADDR_HI) == 0U &&
           cb(rec, AAR_CB_FAULT_ADDR_LO) == 0U);
    assert(cb(rec, AAR_CB_FAULT_PC_LO) == 0x9000u);
    assert(cb(rec, AAR_CB_FAULT_SP_LO) == sp);
    assert(cb(rec, AAR_CB_FAULT_LR_LO) == 0x9100u);
    assert_snapshot(rec, sp, 0x80u);
}

/* A tick that found the app inside the runtime has no user registers: the
 * stall record keeps them zero and there is no snapshot. */
static void test_stall_in_an_api_body_has_no_registers(void)
{
    uint32_t pid;
    zzapp_rec_t *rec;
    zzapp_arm_frame_t frame;

    boot();
    rec = start_app(&pid, 0U);
    zzapp_kernel_set_current_thread((int)pid);
    zzapp_host_tick_at(31u * SECOND_US);
    memset(&frame, 0, sizeof(frame));
    frame.spsr = 0x1fu;
    frame.exception_lr = 0xc0001004u;
    frame.user_sp = 0xc0f00000u;
    frame.user_lr = 0xc0001000u;
    (void)zzapp_kernel_arch_dispatch(&frame, ZZAPP_KERNEL_DISPATCH_TICK, 4u);
    zzapp_worker_hook();
    assert(cb(rec, AAR_CB_END_REASON) == AAR_END_STALL);
    assert(cb(rec, AAR_CB_FAULT_CLASS) == AAR_FAULT_NONE);
    assert(cb(rec, AAR_CB_FAULT_PC_LO) == 0U);
    assert(cb(rec, AAR_CB_FAULT_SP_LO) == 0U);
    assert(cb(rec, AAR_CB_FAULT_LR_LO) == 0U);
    assert_snapshot(rec, 0U, 0U);
}

/* A damaged canary is found by the system thread, which has the frame the
 * scheduler saved. */
static void test_canary_fault_takes_the_saved_frame(void)
{
    uint32_t pid;
    zzapp_rec_t *rec;
    uint32_t sp = WIN_CARD + STACK_LO + 0x4000u;
    uint32_t i;

    boot();
    rec = start_app(&pid, 0U);
    for (i = 0U; i < ZZAPP_CANARY_BYTES; i++)
        window[STACK_LO + i] = 0xA5u;
    window[STACK_LO + 3u] = 0u;
    rec->host.stack_canary = STACK_LO;
    rec->thread.context.frame.spsr = ZZAPP_KERNEL_APP_CPSR;
    rec->thread.context.frame.exception_lr = 0xa000u;
    rec->thread.context.frame.user_sp = sp;
    rec->thread.context.frame.user_lr = 0xa100u;
    zzapp_worker_hook();
    assert(cb(rec, AAR_CB_END_REASON) == AAR_END_USER_FAULT);
    assert(cb(rec, AAR_CB_FAULT_CLASS) == AAR_FAULT_DATA);
    assert(cb(rec, AAR_CB_FAULT_PC_LO) == 0xa000u);
    assert(cb(rec, AAR_CB_FAULT_SP_LO) == sp);
    assert(cb(rec, AAR_CB_FAULT_LR_LO) == 0xa100u);
    assert_snapshot(rec, sp, AAR_CB_STACK_MAX);
}

/* ---- The profiler -------------------------------------------------------- */

/* One scheduler tick that interrupted the app of `pid` in `mode`. */
static void tick(uint32_t pid, uint32_t mode, uint32_t pc)
{
    zzapp_arm_frame_t frame;

    memset(&frame, 0, sizeof(frame));
    frame.spsr = mode;
    frame.exception_lr = pc + 4u;
    frame.user_sp = WIN_CARD + STACK_HI - 16u;
    zzapp_kernel_set_current_thread((int)pid);
    (void)zzapp_kernel_arch_dispatch(&frame, ZZAPP_KERNEL_DISPATCH_TICK, 4u);
}

static uint64_t ring_sample(uint32_t n)
{
    return aar_get_be64((const volatile uint8_t *)ring_ptr +
                        (size_t)(n & (ZZAPP_PROFILE_RING_BYTES / 8U - 1U)) *
                                AAR_PROFILE_SAMPLE_BYTES);
}

static void test_grant_is_a_divisor_of_the_tick_and_never_above_the_ask(void)
{
    static const struct { uint32_t asked, granted; } cases[] = {
        { 1U, 1U }, { 2U, 2U }, { 3U, 2U }, { 70U, 50U }, { 99U, 50U },
        { 100U, 100U }, { 150U, 100U }, { 199U, 100U }, { 200U, 200U },
        { 1000U, 200U }, { 512U, 200U }, { 10000U, 200U },
    };
    unsigned i;

    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        zzapp_rec_t *rec;

        boot();
        rec = start_app(NULL, cases[i].asked);
        assert(cb(rec, AAR_CB_PROFILE_GRANTED_HZ) == cases[i].granted);
        assert(cases[i].granted <= cases[i].asked);
        assert(cb(rec, AAR_CB_PROFILE_BYTES) == ZZAPP_PROFILE_RING_BYTES);
        assert(cb(rec, AAR_CB_PROFILE_TOKEN) == ring_card);
        assert(cb(rec, AAR_CB_PROFILE_HEAD) == 0U);
        assert(cb(rec, AAR_CB_PROFILE_DROPPED) == 0U);
        assert(x_allocs == 2U);
        assert(zzapp_xport_shared_used(rec) ==
               ZZAPP_XPORT_PAIR_ALLOC + ZZAPP_PROFILE_RING_BYTES);
    }
}

static void test_nothing_is_advertised_without_a_grant(void)
{
    zzapp_rec_t *rec;

    boot();
    /* Off, and above the maximum, allocate nothing. */
    rec = start_app(NULL, 0U);
    assert(cb(rec, AAR_CB_PROFILE_GRANTED_HZ) == 0U);
    assert(cb(rec, AAR_CB_PROFILE_TOKEN) == 0U);
    assert(cb(rec, AAR_CB_PROFILE_BYTES) == 0U);
    assert(x_allocs == 1U);
    rec = start_app(NULL, ZZAPP_PROFILE_HZ_MAX + 1U);
    assert(cb(rec, AAR_CB_PROFILE_GRANTED_HZ) == 0U);
    assert(cb(rec, AAR_CB_PROFILE_TOKEN) == 0U);
    assert(x_allocs == 2U);

    /* A refused allocation leaves the words as they were. */
    fail_ring_alloc = 1;
    rec = start_app(NULL, 100U);
    assert(cb(rec, AAR_CB_PROFILE_GRANTED_HZ) == 0U);
    assert(cb(rec, AAR_CB_PROFILE_TOKEN) == 0U);
    assert(cb(rec, AAR_CB_PROFILE_BYTES) == 0U);
    assert(zzapp_xport_shared_used(rec) == ZZAPP_XPORT_PAIR_ALLOC);
    fail_ring_alloc = 0;

    /* So does the claim's shared limit. */
    {
        uint32_t allocs = x_allocs + 1U;
        uint32_t pid = zzapp_test_admit();

        assert(pid != ZZAPP_PID_NONE);
        rec = zzapp_rec_by_pid(pid);
        zzapp_xport_set_shared_limit(rec, ZZAPP_XPORT_PAIR_ALLOC);
        zzapp_xport_cb_put(rec, AAR_CB_PROFILE_HZ, 100U);
        zzapp_loader_bound(WIN_CARD, window, 1U, 1U);
        assert(cb(rec, AAR_CB_PROFILE_GRANTED_HZ) == 0U);
        assert(x_allocs == allocs);
    }
}

static void test_samples_are_the_pc_of_the_interrupted_frame(void)
{
    uint32_t pid;
    zzapp_rec_t *rec;
    uint32_t i;

    boot();
    rec = start_app(&pid, 70U);
    assert(cb(rec, AAR_CB_PROFILE_GRANTED_HZ) == 50U);
    /* 50 Hz of a 200 Hz tick: every fourth tick of the app. */
    for (i = 0U; i < 8U; i++)
        tick(pid, ZZAPP_KERNEL_APP_CPSR, 0x8000u + 4U * i);
    assert(cb(rec, AAR_CB_PROFILE_HEAD) == 2U);
    assert(ring_sample(0U) == 0x800cu);
    assert(ring_sample(1U) == 0x801cu);
    assert(cb(rec, AAR_CB_PROFILE_DROPPED) == 0U);
    /* Never more often than asked: 2 samples in 8 ticks of 5 ms. */
    assert((uint64_t)2U * 200U <= (uint64_t)8U * 70U);

    /* A tick that found the app inside the runtime has no app pc: it is
     * counted, and nothing is written. */
    for (i = 0U; i < 4U; i++)
        tick(pid, 0x1fu, 0xc0001000u);
    assert(cb(rec, AAR_CB_PROFILE_HEAD) == 2U);
    assert(cb(rec, AAR_CB_PROFILE_DROPPED) == 1U);
    assert(ring_sample(2U) == 0U);
    /* Between samples nothing changes. */
    tick(pid, ZZAPP_KERNEL_APP_CPSR, 0x8100u);
    assert(cb(rec, AAR_CB_PROFILE_HEAD) == 2U && cb(rec, AAR_CB_PROFILE_DROPPED) == 1U);
}

static void test_the_ring_wraps_and_the_head_counts(void)
{
    uint32_t pid;
    zzapp_rec_t *rec;
    uint32_t i;
    uint32_t slots = ZZAPP_PROFILE_RING_BYTES / AAR_PROFILE_SAMPLE_BYTES;

    boot();
    rec = start_app(&pid, 1000U);
    assert(cb(rec, AAR_CB_PROFILE_GRANTED_HZ) == 200U);
    for (i = 0U; i < slots + 6U; i++)
        tick(pid, ZZAPP_KERNEL_APP_CPSR, 0x10000u + 4U * i);
    assert(cb(rec, AAR_CB_PROFILE_HEAD) == slots + 6U);
    /* Sample n is at slot n mod slots: the first six slots hold the newest. */
    assert(ring_sample(0U) == 0x10000u + 4U * slots);
    assert(ring_sample(5U) == 0x10000u + 4U * (slots + 5U));
    assert(ring_sample(6U) == 0x10000u + 4U * 6U);
}

static void test_a_second_load_replaces_the_ring_and_release_frees_it(void)
{
    uint32_t pid;
    zzapp_rec_t *rec;

    boot();
    rec = start_app(&pid, 200U);
    assert(x_allocs == 2U && x_frees == 0U);
    tick(pid, ZZAPP_KERNEL_APP_CPSR, 0x8000u);
    assert(cb(rec, AAR_CB_PROFILE_HEAD) == 1U);

    zzapp_xport_cb_put(rec, AAR_CB_PROFILE_HZ, 10U);
    zzapp_loader_bound(WIN_CARD, window, 1U, 2U);
    assert(x_allocs == 3U && x_frees == 1U);
    assert(cb(rec, AAR_CB_PROFILE_GRANTED_HZ) == 10U);
    assert(cb(rec, AAR_CB_PROFILE_HEAD) == 0U);
    assert(zzapp_xport_shared_used(rec) ==
           ZZAPP_XPORT_PAIR_ALLOC + ZZAPP_PROFILE_RING_BYTES);

    /* A load without a rate leaves no ring. */
    zzapp_xport_cb_put(rec, AAR_CB_PROFILE_HZ, 0U);
    zzapp_loader_bound(WIN_CARD, window, 1U, 3U);
    assert(x_frees == 2U);
    assert(cb(rec, AAR_CB_PROFILE_GRANTED_HZ) == 0U);
    assert(zzapp_xport_shared_used(rec) == ZZAPP_XPORT_PAIR_ALLOC);

    zzapp_xport_cb_put(rec, AAR_CB_PROFILE_HZ, 50U);
    zzapp_loader_bound(WIN_CARD, window, 1U, 4U);
    assert(cb(rec, AAR_CB_PROFILE_GRANTED_HZ) == 50U);
    /* The claim's release returns it, once. */
    assert(zzapp_xport_release(rec) == 0);
    assert(zzapp_xport_release(rec) == 0);
    assert(x_allocs == x_frees);
    zzapp_xport_profile_sample(rec, 1, 0x8000u);
}

static void test_the_table_is_bounded_and_a_slot_comes_back(void)
{
    zzapp_rec_t *recs[ZZAPP_PROFILE_SLOTS + 1U];
    unsigned i;

    boot();
    for (i = 0U; i < ZZAPP_PROFILE_SLOTS + 1U; i++)
        recs[i] = start_app(NULL, 200U);
    for (i = 0U; i < ZZAPP_PROFILE_SLOTS; i++)
        assert(cb(recs[i], AAR_CB_PROFILE_GRANTED_HZ) == 200U);
    assert(cb(recs[ZZAPP_PROFILE_SLOTS], AAR_CB_PROFILE_GRANTED_HZ) == 0U);
    assert(cb(recs[ZZAPP_PROFILE_SLOTS], AAR_CB_PROFILE_TOKEN) == 0U);
    assert(zzapp_xport_release(recs[3]) == 0);
    zzapp_xport_cb_put(recs[ZZAPP_PROFILE_SLOTS], AAR_CB_PROFILE_HZ, 200U);
    zzapp_services_select_pid(recs[ZZAPP_PROFILE_SLOTS]->hdr.pid);
    zzapp_loader_bound(WIN_CARD, window, 1U, 2U);
    assert(cb(recs[ZZAPP_PROFILE_SLOTS], AAR_CB_PROFILE_GRANTED_HZ) == 200U);
    for (i = 0U; i < ZZAPP_PROFILE_SLOTS + 1U; i++)
        (void)zzapp_xport_release(recs[i]);
    assert(x_allocs == x_frees);
}

int main(void)
{
    test_fault_snapshot_is_bounded_to_the_stack();
    test_stall_records_where_the_watchdog_found_the_app();
    test_stall_in_an_api_body_has_no_registers();
    test_canary_fault_takes_the_saved_frame();
    test_grant_is_a_divisor_of_the_tick_and_never_above_the_ask();
    test_nothing_is_advertised_without_a_grant();
    test_samples_are_the_pc_of_the_interrupted_frame();
    test_the_ring_wraps_and_the_head_counts();
    test_a_second_load_replaces_the_ring_and_release_frees_it();
    test_the_table_is_bounded_and_a_slot_comes_back();
    printf("fault_profile_test: ok\n");
    return 0;
}
