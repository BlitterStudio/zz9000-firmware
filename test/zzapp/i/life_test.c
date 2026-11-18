/*
 * The app lifecycle on the AAR transport (WP3): the host and the loader write
 * the control block instead of a life block, the lease is read from the lease
 * counter, a client stop word latches the record's own stop, a fault lands in
 * the fault record, a held shell keeps its transport until it is released and
 * a reset returns it, and a quarantined record keeps its own host-visible
 * blocks until its release.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#define ZZAPP_PROTO_NO_WINDOW
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

/* ---- A services layer reduced to what the host calls. ------------------- */

static uint32_t selected;
static int hold_shells;

uint32_t zzapp_services_current_pid(void) { return selected; }
void zzapp_services_select_pid(uint32_t pid) { selected = pid; }
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
/* A record whose client has not released it stays a shell (quarantine and
 * every normal end until RELEASE); without that it is freed at once. */
int zzapp_services_retire_wire(struct zzapp_rec *rec)
{
    if (hold_shells)
        return 0;
    zzapp_xport_cb_set_state(rec, ZZAPP_ST_IDLE, rec->claim.terminal_reason);
    (void)zzapp_xport_release(rec);
    (void)zzapp_rec_free(rec->hdr.handle);
    return 1;
}

/* ---- Counting host-visible heap for the transport. ---------------------- */

static unsigned x_allocs;
static unsigned x_frees;

static int count_alloc(void *ctx, uint32_t bytes, uint32_t align, uint32_t lo,
                       uint32_t hi, void **ptr, uint32_t *card)
{
    void *mem = NULL;

    (void)ctx;
    (void)lo;
    (void)hi;
    if (posix_memalign(&mem, align, bytes) != 0)
        return -1;
    *ptr = mem;
    *card = 0x04000000u + ++x_allocs * 0x100000u;
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

static void boot(void)
{
    host_card_pool_reset(256U << 20);
    zzapp_test_set_arena_apps(64U);
    zzapp_boot_init();
    zzapp_xport_init(&counting_env);
    zzapp_test_set_xtime(0U);
    selected = 0U;
    hold_shells = 0;
    x_allocs = 0U;
    x_frees = 0U;
}

static zzapp_rec_t *admit(uint32_t *pid_out)
{
    uint32_t pid = zzapp_test_admit();

    assert(pid != ZZAPP_PID_NONE);
    if (pid_out)
        *pid_out = pid;
    return zzapp_rec_by_pid(pid);
}

static uint32_t cb(const zzapp_rec_t *rec, uint32_t off)
{
    return zzapp_xport_cb_get(rec, off);
}

/* ---- The claim and the lifecycle words ---------------------------------- */

static void test_claim_and_state_words(void)
{
    uint32_t pid;
    zzapp_rec_t *rec;

    boot();
    rec = admit(&pid);
    assert(zzapp_xport_live(rec));
    assert(cb(rec, AAR_CB_HANDLE) == rec->hdr.handle);
    assert(cb(rec, AAR_CB_STATE) == AAR_STATE_CLAIMED);
    zzapp_loader_set_state(ZZAPP_ST_LOADING);
    assert(cb(rec, AAR_CB_STATE) == AAR_STATE_LOADING);
    zzapp_loader_set_state(ZZAPP_ST_STARTING);
    assert(cb(rec, AAR_CB_STATE) == AAR_STATE_STARTING);
    /* IDLE is the host's own word: the control block turns IDLE only at the
     * retire that ends the claim. */
    zzapp_loader_set_state(ZZAPP_ST_IDLE);
    assert(cb(rec, AAR_CB_STATE) == AAR_STATE_STARTING);
}

/* ---- Lease from the lease counter --------------------------------------- */

static int lease_lost(uint32_t pid)
{
    zzapp_services_select_pid(pid);
    return zzapp_host_stop_requested();
}

/* Advance the clock, keep core 1's own stamp fresh, and run core 0's poll. */
static void poll_at(uint64_t now)
{
    zzapp_test_set_xtime(now);
    zzapp_test_system_tick_at(now);
    zzapp_core0_poll();
}

static void test_lease_counter(void)
{
    uint32_t short_pid, ext_pid, dead_pid;
    zzapp_rec_t *rec_short, *rec_ext, *rec_dead;
    uint64_t t;
    unsigned k;

    boot();
    rec_short = admit(&short_pid);
    zzapp_test_mark_running();
    rec_ext = admit(&ext_pid);
    rec_ext->claim.client_caps = ZZAPP_CLIENT_LEASE_EXTENDED;
    zzapp_test_mark_running();
    rec_dead = admit(&dead_pid);
    zzapp_test_mark_running();

    /* One renewal every second: the short lease never runs out, and the
     * extended one does not either. The dead client never writes the word. */
    poll_at(1000U);
    for (k = 1U; k <= 6U; k++) {
        t = 1000U + (uint64_t)k * 1000000U;
        zzapp_xport_cb_put(rec_short, AAR_CB_LEASE_COUNTER, k);
        zzapp_xport_cb_put(rec_ext, AAR_CB_LEASE_COUNTER, k);
        poll_at(t);
        assert(!lease_lost(short_pid));
        assert(!lease_lost(ext_pid));
        if (k * 1000000U > ZZAPP_LEASE_US)
            assert(lease_lost(dead_pid));
    }
    /* The dead record lost its lease after the short period, with the reason
     * LEASE, and the client's own stop word was left alone. */
    assert(rec_dead->host.terminal_reason == ZZAPP_END_LEASE);
    assert(cb(rec_dead, AAR_CB_STOP_REQUEST) == 0U);

    /* The extended lease holds for a client that stalls for 5 s and runs out
     * after 10 s. */
    poll_at(6000000U + 1000U + 5000000U);
    assert(!lease_lost(ext_pid));
    assert(!lease_lost(short_pid) || rec_short->host.terminal_reason ==
                                     ZZAPP_END_LEASE);
    poll_at(6000000U + 1000U + ZZAPP_LAUNCHER_LEASE_US + 1U);
    assert(lease_lost(ext_pid));
    assert(rec_ext->host.terminal_reason == ZZAPP_END_LEASE);
}

/* ---- The client's stop word ---------------------------------------------- */

static void test_client_stop_word_latches_the_stop(void)
{
    uint32_t pid;
    zzapp_rec_t *rec;

    boot();
    rec = admit(&pid);
    zzapp_services_select_pid(pid);
    assert(!zzapp_host_stop_requested());
    zzapp_xport_cb_put(rec, AAR_CB_STOP_REQUEST, 1U);
    zzapp_worker_hook();
    zzapp_services_select_pid(pid);
    assert(zzapp_host_stop_requested());
}

/* ---- A fault lands in the fault record ----------------------------------- */

static void check_fault(uint32_t fault, uint32_t fsr, uint32_t aar_class)
{
    uint32_t pid;
    zzapp_rec_t *rec;

    hold_shells = 1;
    rec = admit(&pid);
    zzapp_test_mark_running();
    zzapp_take_app_fault(fault, 0x1000u, 0x2000u, 0x3000u, 0x4000u, fsr,
                         0x5000u);
    zzapp_worker_hook();
    assert(cb(rec, AAR_CB_FAULT_CLASS) == aar_class);
    assert(cb(rec, AAR_CB_FAULT_SYNDROME) == fsr);
    assert(cb(rec, AAR_CB_FAULT_PSTATE) == 0x5000u);
    assert(cb(rec, AAR_CB_FAULT_PC_LO) == 0x1000u);
    assert(cb(rec, AAR_CB_FAULT_SP_LO) == 0x2000u);
    assert(cb(rec, AAR_CB_FAULT_LR_LO) == 0x3000u);
    assert(cb(rec, AAR_CB_FAULT_ADDR_LO) == 0x4000u);
    assert(cb(rec, AAR_CB_STATE) == AAR_STATE_FAULTED);
    assert(cb(rec, AAR_CB_END_REASON) == AAR_END_USER_FAULT);
    /* The shell is held with its transport: the client can still read. */
    assert(zzapp_rec_state(pid) == ZZAPP_REC_RECLAIMED);
    assert(zzapp_xport_live(rec));
}

static void test_fault_record(void)
{
    boot();
    check_fault(ZZAPP_FAULT_DATA, 0x805u, AAR_FAULT_DATA);
    check_fault(ZZAPP_FAULT_DATA, 0x001u, AAR_FAULT_ALIGN);
    check_fault(ZZAPP_FAULT_PREFETCH, 0x00du, AAR_FAULT_FETCH);
    check_fault(ZZAPP_FAULT_UNDEF, 0u, AAR_FAULT_ILLEGAL);
}

/* ---- Held shells and reset ----------------------------------------------- */

static void test_reset_releases_held_shells(void)
{
    uint32_t a, b, c;
    zzapp_rec_t *rec_a, *rec_b, *rec_c;
    zzapp_xport_buf_t buf;
    zzapp_rec_stats_t stats;

    boot();
    hold_shells = 1;
    rec_a = admit(&a);
    rec_b = admit(&b);
    rec_c = admit(&c);
    assert(zzapp_xport_service_area(rec_a, ZZAPP_XPORT_AREA_SCRATCH, &buf) ==
           AAR_OK);
    assert(zzapp_xport_service_area(rec_b, ZZAPP_XPORT_AREA_LOG, &buf) ==
           AAR_OK);
    zzapp_services_select_pid(a);
    assert(zzapp_rec_retire(a) == 0);
    zzapp_services_select_pid(b);
    assert(zzapp_rec_retire(b) == 0);
    /* a and b are shells holding pair and service block; c is still live. */
    assert(zzapp_rec_state(a) == ZZAPP_REC_RECLAIMED);
    assert(zzapp_rec_state(b) == ZZAPP_REC_RECLAIMED);
    assert(zzapp_xport_live(rec_a) && zzapp_xport_live(rec_b));
    assert(x_allocs == 5U && x_frees == 0U);
    zzapp_rec_stats(&stats);
    assert(stats.free == stats.capacity - 3U);

    /* The reset retires c, which stays a shell too (services holds every
     * shell), and releases the held shells: every block goes back, once,
     * and no record is left. */
    zzapp_on_amiga_reset();
    assert(zzapp_rec_state(a) == ZZAPP_REC_FREE);
    assert(zzapp_rec_state(b) == ZZAPP_REC_FREE);
    assert(zzapp_rec_state(c) == ZZAPP_REC_FREE);
    assert(!zzapp_xport_live(rec_a) && !zzapp_xport_live(rec_b) &&
           !zzapp_xport_live(rec_c));
    assert(x_frees == x_allocs);
    zzapp_rec_stats(&stats);
    assert(stats.free == stats.capacity);
}

/* ---- Quarantine keeps the app's own blocks until the release ------------- */

static uint32_t shared_used(void)
{
    struct aar_info info;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    assert(zzapp_host_info(&info) == AAR_OK);
    return (uint32_t)info.shared_used;
}

static void test_quarantine_keeps_visible_blocks(void)
{
    uint32_t plain, held;
    zzapp_rec_t *rec_plain, *rec_held;
    void *block;

    boot();
    hold_shells = 1;
    rec_plain = admit(&plain);
    block = zzapp_host_alloc(4096u, 4096u);
    assert(block);
    assert(shared_used() == 4096u + ZZAPP_XPORT_PAIR_ALLOC);
    assert(zzapp_xport_shared_used(rec_plain) == ZZAPP_XPORT_PAIR_ALLOC);
    assert(zzapp_rec_retire(plain) == 0);
    /* A normal end returns the app's own blocks and keeps the transport. */
    assert(shared_used() == ZZAPP_XPORT_PAIR_ALLOC);

    rec_held = admit(&held);
    block = zzapp_host_alloc(8192u, 4096u);
    assert(block);
    assert(shared_used() == 8192u + ZZAPP_XPORT_PAIR_ALLOC);
    zzapp_xport_flag_set(rec_held, ZZAPP_XPORT_F_QUARANTINED);
    assert(zzapp_rec_retire(held) == 0);
    /* A quarantined record keeps them: its client may still write. The
     * release of the shell returns them. */
    assert(shared_used() == 8192u + ZZAPP_XPORT_PAIR_ALLOC);
    zzapp_host_revoke_visible();
    assert(shared_used() == ZZAPP_XPORT_PAIR_ALLOC);
}

int main(void)
{
    test_claim_and_state_words();
    test_lease_counter();
    test_client_stop_word_latches_the_stop();
    test_fault_record();
    test_reset_releases_held_shells();
    test_quarantine_keeps_visible_blocks();
    printf("life_test: ok\n");
    return 0;
}
