/*
 * The lifecycle of an app on its process record (slice 5b): the staging block
 * is released at LOAD_END, a commit failure at LOAD_END returns everything it
 * took, the single retire path can be halted after each stage and replayed
 * to the same counters, a bad record index is a hard failure, and a reset
 * with many live records waits on one shared deadline.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aar/app.h"
#include "aar_note_image.h"
#include "card_pool.h"
#include "host_card_pool.h"
#include "xil_cache.h"
#include "zzapp_as.h"
#include "zzapp_rec.h"
#include "zzapp_runtime.h"

#define MB(n) ((uint32_t)(n) << 20)

int zzapp_loader_have_image(void);
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
static uint32_t last_refusal;
static unsigned on_reset_calls;

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
void zzapp_services_note_refusal(uint32_t reason) { last_refusal = reason; }
void zzapp_services_on_reset(void)
{
    zzapp_rec_stats_t records;
    zzapp_as_stats_t tables;

    /* Services must not forget the windows before every record is retired. */
    zzapp_rec_stats(&records);
    zzapp_as_stats(&tables);
    assert(records.free == records.capacity);
    assert(tables.l2_reserved == 0U);
    on_reset_calls++;
}

/* The surface allocator model: every host-visible block is counted in and
 * out, so a replayed retire that frees one twice is seen. */
static unsigned card_allocs;
static unsigned card_frees;

uint32_t zzapp_test_card_alloc(uint32_t bytes, uint32_t align,
                               uint32_t *serial)
{
    (void)bytes;
    (void)align;
    *serial = ++card_allocs;
    return 0x0a000000u + card_allocs * 0x10000u;
}

void zzapp_test_card_free(uint32_t card, uint32_t serial)
{
    (void)card;
    (void)serial;
    card_frees++;
}

/* ---- Images and counters. ----------------------------------------------- */

#define IMAGE_BYTES 0x200u
#define PHOFF 52u
#define LOAD_OFF 0x100u
#define DYN_OFF 0x0e0u
#define RELA_OFF 0x0d0u

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* One PT_LOAD from file offset 0, one AAR note, and a DT_RELA table with one
 * relocation, which sits inside the segment. `heap_mib` of private heap. */
static void make_image(uint8_t image[IMAGE_BYTES], uint32_t heap_mib)
{
    uint8_t *ph = image + PHOFF;

    memset(image, 0, IMAGE_BYTES);
    image[0] = 0x7f; image[1] = 'E'; image[2] = 'L'; image[3] = 'F';
    image[4] = 1; image[5] = 1; image[6] = 1;
    put16(image + 16, 3);                 /* ET_DYN */
    put16(image + 18, 40);                /* EM_ARM */
    put32(image + 24, LOAD_OFF);
    put32(image + 28, PHOFF);
    put32(image + 36, 0x400);             /* hard-float */
    put16(image + 40, 52);
    put16(image + 42, 32);
    put16(image + 44, 3);
    put16(image + 46, 40);
    put32(ph + 0, 1);                     /* PT_LOAD */
    put32(ph + 16, LOAD_OFF + 4U);
    put32(ph + 20, LOAD_OFF + 8U);
    put32(ph + 24, 1);                    /* PF_X */
    ph += 32;
    put32(ph + 0, 4);                     /* PT_NOTE */
    put32(ph + 4, TEST_NOTE_OFF);
    put32(ph + 16, TEST_NOTE_BYTES);
    ph += 32;
    put32(ph + 0, 2);                     /* PT_DYNAMIC */
    put32(ph + 4, DYN_OFF);
    put32(ph + 8, DYN_OFF);
    put32(ph + 16, 32);
    put32(ph + 20, 32);
    put32(ph + 24, 4);
    test_note_init(image + TEST_NOTE_OFF);
    test_note_set(image + TEST_NOTE_OFF, TEST_NOTE_W_MIN_HEAP_MIB, heap_mib);
    put32(image + RELA_OFF + 0, LOAD_OFF);
    put32(image + RELA_OFF + 4, 23);      /* R_ARM_RELATIVE */
    put32(image + RELA_OFF + 8, LOAD_OFF);
    put32(image + DYN_OFF + 0, 7);        /* DT_RELA */
    put32(image + DYN_OFF + 4, RELA_OFF);
    put32(image + DYN_OFF + 8, 8);        /* DT_RELASZ */
    put32(image + DYN_OFF + 12, 12);
    put32(image + DYN_OFF + 16, 9);       /* DT_RELAENT */
    put32(image + DYN_OFF + 20, 12);
}

/* Claim a record, as services does, and run LOAD_BEGIN, the upload and LOAD_END
 * to its end. Returns the LOAD_END result; *pid_out is the record. */
static int load(uint32_t heap_mib, uint32_t *pid_out)
{
    uint8_t image[IMAGE_BYTES];
    uint32_t pid = zzapp_test_admit();
    int rc;

    assert(pid != ZZAPP_PID_NONE);
    *pid_out = pid;
    make_image(image, heap_mib);
    assert(zzapp_loader_begin() == AAR_OK);
    assert(zzapp_loader_chunk(0, image, IMAGE_BYTES) == AAR_OK);
    do {
        rc = zzapp_loader_end();
    } while (rc == ZZAPP_LOADER_IN_PROGRESS);
    return rc;
}

struct counters {
    uint32_t pool_used;
    uint32_t ledger_free;
    uint32_t class_app;
    uint32_t class_display;
    zzapp_as_stats_t as;
    zzapp_rec_stats_t rec;
};

static void take(struct counters *c)
{
    memset(c, 0, sizeof(*c));
    c->pool_used = host_card_pool_used();
    c->ledger_free = zzapp_loader_pool_free();
    c->class_app = card_pool.class_used[CARD_POOL_CLASS_APP];
    c->class_display = card_pool.class_used[CARD_POOL_CLASS_APP_DISPLAY];
    zzapp_as_stats(&c->as);
    zzapp_rec_stats(&c->rec);
}

/* Everything a claim can take is back: the pool, the ledger, the tables and
 * their reserve, the ASIDs and the record slots. */
static void same(const struct counters *a, const struct counters *b)
{
    assert(a->pool_used == b->pool_used);
    assert(a->ledger_free == b->ledger_free);
    assert(a->class_app == b->class_app);
    assert(a->class_display == b->class_display);
    assert(a->as.l2_free == b->as.l2_free);
    assert(a->as.l2_reserved == b->as.l2_reserved);
    assert(a->as.l2_reserved_unused == b->as.l2_reserved_unused);
    assert(a->as.l2_shared_free == b->as.l2_shared_free);
    assert(a->as.asid_assigned == b->as.asid_assigned);
    assert(a->rec.free == b->rec.free);
    assert(a->rec.live == b->rec.live);
    assert(a->rec.claimed == b->rec.claimed);
    assert(a->rec.retiring == b->rec.retiring);
}

static void boot(void)
{
    host_card_pool_reset(256U << 20);
    zzapp_test_set_arena_apps(64U);
    zzapp_boot_init();
    selected = 0U;
    card_allocs = 0U;
    card_frees = 0U;
}

/* ---- (a) staging is released at LOAD_END -------------------------------- */

static void test_staging_released_at_load_end(void)
{
    struct counters start;
    uint32_t pids[8];
    uint32_t serials[8];
    unsigned k;

    boot();
    take(&start);
    assert(start.class_app == 0U);
    for (k = 0U; k < 8U; k++) {
        zzapp_rec_t *rec;
        unsigned j;

        pids[k] = zzapp_test_admit();
        assert(pids[k] == k);
        rec = zzapp_rec_by_pid(pids[k]);
        serials[k] = rec->hdr.owner_serial;
        assert(serials[k] != 0U);
        for (j = 0U; j < k; j++)
            assert(serials[j] != serials[k]);
        {
            uint8_t image[IMAGE_BYTES];
            int rc;

            make_image(image, 1U);
            assert(zzapp_loader_begin() == AAR_OK);
            /* The staging block is a 16 MiB class APP block owned by the
             * record's serial, with no slot bits in the owner id. */
            assert(card_pool.class_used[CARD_POOL_CLASS_APP] == ZZAPP_FILE_MAX);
            {
                uint32_t b;
                int found = 0;

                for (b = 0U; b < card_pool.block_count; b++) {
                    if (CARD_POOL_OWNER_CLASS(card_pool.blocks[b].owner) ==
                        CARD_POOL_CLASS_APP) {
                        assert(card_pool.blocks[b].owner ==
                               CARD_POOL_OWNER(CARD_POOL_CLASS_APP,
                                               serials[k]));
                        found = 1;
                    }
                }
                assert(found);
            }
            assert(zzapp_loader_chunk(0, image, IMAGE_BYTES) == AAR_OK);
            do {
                rc = zzapp_loader_end();
            } while (rc == ZZAPP_LOADER_IN_PROGRESS);
            assert(rc == AAR_OK);
        }
        /* Loaded: the block is gone, the app keeps only its frames. */
        assert(card_pool.class_used[CARD_POOL_CLASS_APP] == 0U);
        assert(zzapp_loader_state() == ZZAPP_ST_LOADING);
        assert(zzapp_loader_have_image());
        assert(zzapp_proc_slot(pids[k])->memory.committed == MB(3));
    }
    /* Eight live apps with 16 MiB of staging each would be 128 MiB against a
     * 96 MiB class limit: they only fit because staging does not live on. */
    assert(zzapp_rec_by_pid(0U)->proc.memory.committed == MB(3));
    for (k = 0U; k < 8U; k++) {
        selected = pids[k];
        assert(zzapp_rec_retire(pids[k]) == 1);
    }
    {
        struct counters end;

        take(&end);
        same(&start, &end);
    }
}

/* ---- (b) a commit failure at LOAD_END returns everything ---------------- */

static void test_commit_failure_at_load_end_releases_everything(void)
{
    struct counters start;
    struct counters held;
    struct counters end;
    zzapp_as_stats_t as;
    uint32_t pid;
    int rc;

    /* 128 MiB of system reserve, the 16 MiB staging block, and room for
     * three of the four frames the image needs (image, two heap, stack):
     * the stack frame is the one the ledger refuses. */
    host_card_pool_reset(MB(128 + 16 + 3));
    zzapp_test_set_arena_apps(64U);
    zzapp_boot_init();
    take(&start);
    last_refusal = 0U;
    rc = load(2U, &pid);
    assert(rc == AAR_E_NOMEM);
    assert(last_refusal == AAR_REASON_MEMORY);
    /* The loader dropped what it held (staging, frames, the address space)
     * and kept the claim: the record and its L2 reserve remain. */
    take(&held);
    assert(held.class_app == 0U);
    assert(held.ledger_free == start.ledger_free);
    assert(held.pool_used == start.pool_used);
    assert(held.as.l2_reserved == ZZAPP_AS_L2_RESERVE);
    zzapp_as_stats(&as);
    assert(as.l2_free == start.as.l2_free);
    assert(as.l2_shared_free + ZZAPP_AS_L2_RESERVE == start.as.l2_shared_free);
    assert(zzapp_rec_state(pid) == ZZAPP_REC_LIVE);
    assert(zzapp_proc_slot(pid)->as.active == 0U);
    {
        const uint32_t *l1 = zzapp_as_pool_l1(pid);
        uint32_t i;

        for (i = 0U; i < ZZAPP_AS_L1_BYTES / 4U; i++)
            assert(l1[i] == 0U);
    }
    assert(zzapp_loader_state() == ZZAPP_ST_IDLE);
    /* The claim ends: the reserve and the slot return too. */
    assert(zzapp_rec_retire(pid) == 1);
    take(&end);
    same(&start, &end);
}

/* A refused LOAD_END ends the load, not the claim: the same record takes a
 * new LOAD_BEGIN, and its tables and L1 start clean. */
static void test_load_can_be_retried_on_the_same_claim(void)
{
    struct counters start;
    struct counters end;
    uint8_t image[IMAGE_BYTES];
    uint32_t pid;
    int rc;

    boot();
    take(&start);
    pid = zzapp_test_admit();
    make_image(image, 1U);
    image[1] = 'X';                       /* not an ELF */
    assert(zzapp_loader_begin() == AAR_OK);
    assert(zzapp_loader_chunk(0, image, IMAGE_BYTES) == AAR_OK);
    do {
        rc = zzapp_loader_end();
    } while (rc == ZZAPP_LOADER_IN_PROGRESS);
    assert(rc == AAR_E_INVAL);
    assert(card_pool.class_used[CARD_POOL_CLASS_APP] == 0U);
    assert(zzapp_rec_state(pid) == ZZAPP_REC_LIVE);
    make_image(image, 1U);
    assert(zzapp_loader_begin() == AAR_OK);
    assert(zzapp_loader_chunk(0, image, IMAGE_BYTES) == AAR_OK);
    do {
        rc = zzapp_loader_end();
    } while (rc == ZZAPP_LOADER_IN_PROGRESS);
    assert(rc == AAR_OK);
    assert(zzapp_proc_slot(pid)->as.active);
    assert(zzapp_rec_retire(pid) == 1);
    take(&end);
    same(&start, &end);
}

/* ---- (c) the retire can be halted after each stage and replayed --------- */

static void expect_partial(unsigned stage, uint32_t pid,
                           const struct counters *start)
{
    struct counters now;
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

    take(&now);
    assert(zzapp_rec_state(pid) == ZZAPP_REC_RETIRING);
    assert(rec->hdr.retire_stage >= stage || stage > ZZAPP_RETIRE_COMMIT);
    switch (stage) {
    case ZZAPP_RETIRE_DETACHED:
        /* Only the scheduler detach ran: everything is still held. */
        assert(now.ledger_free < start->ledger_free);
        assert(now.class_display > start->class_display);
        break;
    case ZZAPP_RETIRE_RESOURCES:
        /* The back buffer and the host-visible block are back, the frames
         * and the address space are not. */
        assert(now.class_display == start->class_display);
        assert(card_frees == card_allocs);
        assert(now.ledger_free < start->ledger_free);
        assert(now.as.l2_reserved == ZZAPP_AS_L2_RESERVE);
        break;
    case ZZAPP_RETIRE_TABLES:
        /* The tables and the reserve are back, the frames are not. */
        assert(now.as.l2_free == start->as.l2_free);
        assert(now.as.l2_reserved == 0U);
        assert(now.as.asid_assigned == 0U);
        assert(now.ledger_free < start->ledger_free);
        break;
    case ZZAPP_RETIRE_COMMIT:
        /* Only the window detach is left. */
        assert(now.ledger_free == start->ledger_free);
        assert(now.as.l2_free == start->as.l2_free);
        break;
    default:
        break;
    }
}

/* A claim with everything a running app can hold: frames, the image's L2
 * tables, an ASID, a host-visible block, a back buffer. */
static uint32_t load_with_resources(void)
{
    uint32_t pid;
    uint32_t back;
    zzapp_proc_t *proc;

    assert(load(1U, &pid) == AAR_OK);
    proc = zzapp_proc_slot(pid);
    assert(proc->as.active);
    assert(zzapp_as_acquire_asid(&proc->as) > 0);
    assert(zzapp_loader_alloc_back(0x10000U, &back) == AAR_OK);
    assert(zzapp_host_alloc(4096U, 4096U) != NULL);
    return pid;
}

static void replay_after(unsigned stage)
{
    struct counters start;
    struct counters end;
    uint32_t pid;
    zzapp_thread_t *thread;

    boot();
    take(&start);
    pid = load_with_resources();
    assert(zzapp_test_cold_restarts() == 0);
    zzapp_test_retire_halt_stage = stage;
    assert(zzapp_rec_retire(pid) == 0);
    zzapp_test_retire_halt_stage = 0U;
    expect_partial(stage, pid, &start);
    /* Core 1 restarts: recovery retires every record again, from the start. */
    zzapp_recover_halted_core1();
    assert(zzapp_test_cold_restarts() == 1);
    assert(zzapp_rec_state(pid) == ZZAPP_REC_FREE);
    thread = zzapp_kernel_thread(zzapp_runtime_kernel(), pid);
    assert(thread->state == ZZAPP_THREAD_STOPPED);
    take(&end);
    same(&start, &end);
    /* The host-visible block went back exactly once. */
    assert(card_allocs == 1U && card_frees == 1U);
}

/* The record layer's own stages: a halt inside the tables stage and inside
 * the free. */
static void replay_after_inner_step(unsigned step, int in_free)
{
    struct counters start;
    struct counters end;
    uint32_t pid;
    zzapp_rec_t *rec;

    boot();
    take(&start);
    pid = load_with_resources();
    rec = zzapp_rec_by_pid(pid);
    zzapp_rec_test_halt_step = step;
    if (in_free) {
        /* Stop after the retire reached the window: only the free halts. */
        zzapp_rec_test_halt_step = 0U;
        zzapp_test_retire_halt_stage = ZZAPP_RETIRE_COMMIT;
        assert(zzapp_rec_retire(pid) == 0);
        zzapp_test_retire_halt_stage = 0U;
        zzapp_rec_test_halt_step = step;
        selected = pid;
        (void)zzapp_services_retire_wire(rec);
    } else {
        assert(zzapp_rec_retire(pid) == 0);
    }
    zzapp_rec_test_halt_step = 0U;
    assert(zzapp_rec_state(pid) != ZZAPP_REC_FREE || in_free);
    zzapp_recover_halted_core1();
    assert(zzapp_rec_state(pid) == ZZAPP_REC_FREE);
    take(&end);
    same(&start, &end);
    assert(card_allocs == 1U && card_frees == 1U);
}

static void test_retire_replay_after_every_stage(void)
{
    unsigned stage;

    for (stage = ZZAPP_RETIRE_DETACHED; stage <= ZZAPP_RETIRE_COMMIT; stage++)
        replay_after(stage);
    replay_after_inner_step(1U, 0);   /* tables: space destroyed */
    replay_after_inner_step(2U, 0);   /* tables: reserve released */
    replay_after_inner_step(1U, 1);   /* free: generation bumped */
    replay_after_inner_step(2U, 1);   /* free: FREE, not queued */
}

/* ---- Hard failure instead of the slot clamp ----------------------------- */

static int aborts(void (*fn)(void))
{
    pid_t child;
    int status = 0;

    fflush(NULL);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        fn();
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

static void api_enter_on_bad_pid(void)
{
    selected = zzapp_rec_capacity() + 3U;
    zzapp_api_enter();
}

static void note_progress_with_bad_pid_is_quiet(void)
{
    /* Without a record of its own the context writes nothing anywhere. */
    selected = ZZAPP_PID_NONE;
    zzapp_note_progress();
}

static void test_bad_record_index_is_a_hard_failure(void)
{
    boot();
    assert(zzapp_proc_slot(zzapp_rec_capacity()) == NULL);
    assert(zzapp_proc_slot(ZZAPP_PID_NONE) == NULL);
    /* A pid outside the arena used to alias slot 0. It stops the caller. */
    assert(aborts(api_enter_on_bad_pid));
    assert(!aborts(note_progress_with_bad_pid_is_quiet));
    /* Calls that name a record from outside leave record 0 alone. */
    assert(zzapp_test_admit() == 0U);
    zzapp_host_latch_stop(zzapp_rec_capacity());
    assert(!zzapp_host_take_stop_event(zzapp_rec_capacity()));
    assert(zzapp_host_note_stop_event_delivered(zzapp_rec_capacity()) == 1);
    selected = 0U;
    assert(!zzapp_host_stop_requested());
}

/* ---- The heartbeat is not a field of record 0 --------------------------- */

static void test_core_heartbeat_is_not_in_record_zero(void)
{
    uint32_t pid;

    boot();
    pid = zzapp_test_admit();
    assert(pid == 0U);
    pid = zzapp_test_admit();
    assert(pid == 1U);
    zzapp_test_set_xtime(1234U);
    zzapp_kernel_set_current_thread(1);
    zzapp_test_mark_running();
    zzapp_host_tick_at(5678U);
    zzapp_kernel_set_current_thread(-1);
    assert(zzapp_rec_by_pid(0U)->coh.core_heartbeat == 0U);
    assert(zzapp_rec_by_pid(1U)->coh.tick_seen == 5678U);
}

/* ---- Reset: one shared deadline, every record retired ------------------- */

static void test_reset_deadline_is_shared(void)
{
    assert(zzapp_reset_deadline_us(0U) == 1000000U);
    assert(zzapp_reset_deadline_us(1U) == 1020000U);
    assert(zzapp_reset_deadline_us(4U) == 1080000U);
    assert(zzapp_reset_deadline_us(64U) == 2280000U);
    assert(zzapp_reset_deadline_us(10U) == 1200000U);
    assert(zzapp_reset_deadline_us(100U) == 3000000U);
    /* Capped at 4 s however many records are live, never N seconds. */
    assert(zzapp_reset_deadline_us(150U) == 4000000U);
    assert(zzapp_reset_deadline_us(151U) == 4000000U);
    assert(zzapp_reset_deadline_us(UINT32_MAX) == 4000000U);
    assert(zzapp_reset_deadline_us(115U) == 3300000U);
    assert(zzapp_reset_deadline_us(1024U) == 4000000U);
}

static unsigned core1_steps;
static unsigned core1_step_limit = 64U;

/* The host's weak worker step, replaced: core 1 answers only after `limit`
 * passes, so the reset needs the recovery when it never answers. */
void zzapp_test_core1_step(uint32_t pid, unsigned pass)
{
    (void)pid;
    (void)pass;
    core1_steps++;
    if (core1_steps > core1_step_limit)
        return;
    zzapp_worker_hook();
}

static uint64_t reset_clock;
static uint64_t reset_time(void)
{
    reset_clock += 100U;
    return reset_clock;
}
void zzapp_test_set_time_hook(uint64_t (*hook)(void));

static void test_reset_with_live_records(uint32_t count)
{
    struct counters start;
    struct counters end;
    uint32_t pids[64];
    unsigned k;

    /* One worker pass retires the complete set, not one record per wait. */
    boot();
    take(&start);
    on_reset_calls = 0U;
    for (k = 0U; k < count; k++) {
        uint32_t back;

        pids[k] = zzapp_test_admit();
        assert(pids[k] == k);
        if (k % 2U == 0U) {
            uint8_t image[IMAGE_BYTES];
            int rc;

            make_image(image, 1U);
            assert(zzapp_loader_begin() == AAR_OK);
            assert(zzapp_loader_chunk(0, image, IMAGE_BYTES) == AAR_OK);
            do {
                rc = zzapp_loader_end();
            } while (rc == ZZAPP_LOADER_IN_PROGRESS);
            assert(rc == AAR_OK);
            assert(zzapp_loader_alloc_back(0x10000U, &back) == AAR_OK);
        }
    }
    core1_steps = 0U;
    core1_step_limit = 64U;
    reset_clock = 0;
    zzapp_test_set_time_hook(reset_time);
    zzapp_on_amiga_reset();
    zzapp_test_set_time_hook(NULL);
    assert(core1_steps == (count != 0U));
    assert(reset_clock < 10000U);
    assert(zzapp_test_cold_restarts() == 0);
    assert(on_reset_calls == 1U);
    take(&end);
    same(&start, &end);
    for (k = 0U; k < count; k++)
        assert(zzapp_rec_state(pids[k]) == ZZAPP_REC_FREE);

    /* A core 1 that never answers: the whole set is recovered once. */
    boot();
    take(&start);
    on_reset_calls = 0U;
    for (k = 0U; k < count; k++) {
        pids[k] = zzapp_test_admit();
        assert(pids[k] == k);
    }
    core1_steps = 0U;
    core1_step_limit = 0U;
    reset_clock = 0;
    zzapp_test_set_time_hook(reset_time);
    zzapp_on_amiga_reset();
    zzapp_test_set_time_hook(NULL);
    /* A hundred-microsecond polling quantum may straddle the deadline.
     * Crucially this is one bound for the set, not one bound per record. */
    if (count != 0U) {
        assert(reset_clock >= zzapp_reset_deadline_us(count));
        assert(reset_clock <= zzapp_reset_deadline_us(count) + 1000U);
    } else {
        assert(reset_clock < 1000U);
        assert(core1_steps == 0U);
    }
    core1_step_limit = 64U;
    assert(zzapp_test_cold_restarts() == (count != 0U));
    assert(on_reset_calls == 1U);
    take(&end);
    same(&start, &end);
    for (k = 0U; k < count; k++) {
        zzapp_rec_coherent_t *coh = &zzapp_rec_by_pid(pids[k])->coh;

        assert(zzapp_rec_state(pids[k]) == ZZAPP_REC_FREE);
        assert(coh->reset_ack == 1U && coh->reset_request == 0U);
    }
}

/* ---- Boot without a record arena ---------------------------------------- */

/* The boot carve found no memory: apps are unavailable, yet core 1's shared
 * worker takes the system lock before anything else. The kernel is a static
 * nothing initialised before, so all zero, and zero reads as record 0 owning
 * the API lock. */
static void test_boot_without_arena_leaves_a_usable_kernel(void)
{
    zzapp_kernel_t *kernel = zzapp_runtime_kernel();
    uint32_t handle = 0U;
    zzapp_rec_t *rec = NULL;

    host_card_pool_reset(256U << 20);
    memset(kernel, 0, sizeof(*kernel));
    zzapp_test_set_arena_apps(0U);
    zzapp_boot_init();
    zzapp_test_set_arena_apps(64U);
    assert(zzapp_rec_capacity() == 0U);
    assert(zzapp_kernel_capacity(kernel) == 0U);
    assert(zzapp_kernel_thread(kernel, 0U) == NULL);
    assert(zzapp_kernel_system_lock(kernel) == 0);
    assert(zzapp_kernel_system_unlock(kernel) == 0);
    /* And again: the worker takes it on every pass. */
    assert(zzapp_kernel_system_lock(kernel) == 0);
    assert(zzapp_kernel_system_unlock(kernel) == 0);
    assert(zzapp_rec_admit(&handle, &rec) == AAR_REASON_MEMORY);
}

int main(void)
{
    test_staging_released_at_load_end();
    test_commit_failure_at_load_end_releases_everything();
    test_load_can_be_retried_on_the_same_claim();
    test_retire_replay_after_every_stage();
    test_bad_record_index_is_a_hard_failure();
    test_boot_without_arena_leaves_a_usable_kernel();
    test_core_heartbeat_is_not_in_record_zero();
    test_reset_deadline_is_shared();
    test_reset_with_live_records(0U);
    test_reset_with_live_records(4U);
    test_reset_with_live_records(8U);
    test_reset_with_live_records(64U);
    printf("retire_test: ok\n");
    return 0;
}
