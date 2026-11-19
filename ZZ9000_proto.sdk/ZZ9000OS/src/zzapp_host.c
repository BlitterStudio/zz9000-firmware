/*
 * Trusted app lifetime, watchdog, service tick, and fault state.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef ZZAPP_HOST_TEST
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/mman.h>
#endif
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "aar/app.h"
#include "zzapp_proto.h"
#include "zzapp_state.h"
#include "zzapp_xport.h"
#include "zzapp_runtime.h"
#include "zzapp_rec.h"
#include "zzapp_svc.h"
#ifndef ZZAPP_HOST_TEST
#include <stdio.h>
#include "memorymap.h"
#include "mmu_page.h"
#ifndef ZZAPP_HOST_TEST
#include "scheduler.h"
#include "zzapp_kernel.h"
#endif
#include "xtime_l.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xparameters_ps.h"
#include "xparameters.h"
#include "xscutimer_hw.h"
#include "card_pool.h"
#include "surface_allocator.h"
#include "core2.h"
#include "interrupt.h"
#include "sleep.h"
#else
#include <stdlib.h>
/* ADDR_ADJ: the board-relative token of a shared block is the same
 * expression on the host as on the card. */
#include "memorymap.h"
#endif

extern void zzapp_mem_set_low_memory_callback(
    zzapp_mem_t *memory, zzapp_mem_low_callback_t callback, void *context)
    __attribute__((weak));

_Static_assert(ZZAPP_MEM_REGISTRY_PROCS >= ZZAPP_REC_MAX_APPS,
               "the commit ledger must track a proc for every record");

#ifdef ZZAPP_HOST_TEST
/* Records of the host harness arena (zzapp_boot_init). */
#ifndef ZZAPP_HOST_ARENA_APPS
#define ZZAPP_HOST_ARENA_APPS 64U
#endif
static uint8_t g_test_wait_registered[ZZAPP_REC_MAX_APPS];
static uint32_t g_test_arena_apps = ZZAPP_HOST_ARENA_APPS;
void __attribute__((weak)) scheduler_core1_service_clean_request(void)
{
}

#endif

/* A bad index is a bug in the caller, never "slot 0": stop hard. On the card
 * the undefined instruction takes core 1 through its fault path and the
 * watchdog restart. */
static void hard_fail(void)
{
#ifdef ZZAPP_HOST_TEST
    abort();
#else
    __builtin_trap();
#endif
}

/* The record context of a system thread that services has not selected one
 * for, only used when services is not linked (host harness). */
static uint32_t g_weak_selected;

uint32_t __attribute__((weak)) zzapp_services_current_pid(void)
{
    int pid = zzapp_kernel_current_thread();

    if (pid >= 0 && (uint32_t)pid < zzapp_rec_capacity())
        return (uint32_t)pid;
    return g_weak_selected;
}

void __attribute__((weak)) zzapp_services_select_pid(uint32_t pid)
{
    g_weak_selected = pid;
}

uint32_t __attribute__((weak)) zzapp_services_memory_limit(void)
{
    return 0U;
}

uint32_t __attribute__((weak)) zzapp_services_host_visible_limit(uint32_t pid)
{
    (void)pid;
    return 0U;
}

uint32_t __attribute__((weak)) zzapp_services_state(void)
{
    return ZZAPP_ST_IDLE;
}

void __attribute__((weak)) zzapp_services_set_state(uint32_t state)
{
    zzapp_loader_set_state(state);
}

void __attribute__((weak)) zzapp_services_set_terminal(uint32_t reason)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(zzapp_services_current_pid());

    if (rec)
        rec->claim.terminal_reason = reason;
}

void __attribute__((weak)) zzapp_services_note_refusal(uint32_t reason)
{
    (void)reason;
}

/* Stages 6 and 7 without a services layer: publish IDLE and the end reason in
 * the control block, return the transport (the client has nothing left to
 * read in a harness), then free the record. */
int __attribute__((weak)) zzapp_services_retire_wire(struct zzapp_rec *rec)
{
    zzapp_xport_cb_set_state(rec, ZZAPP_ST_IDLE, rec->claim.terminal_reason);
    (void)zzapp_xport_release(rec);
    (void)zzapp_rec_free(rec->hdr.handle);
    return 1;
}

void __attribute__((weak)) zzapp_services_expire_claims(uint64_t now)
{
    (void)now;
}

int __attribute__((weak)) zzapp_services_wait_registered(uint32_t pid)
{
#ifdef ZZAPP_HOST_TEST
    return pid < ZZAPP_REC_MAX_APPS && g_test_wait_registered[pid];
#else
    (void)pid;
    return 0;
#endif
}

/* The core's heartbeat: core 1 stamps it from its timer, core 0 ages it. It
 * is one global word, not a field of record 0. */
#ifndef ZZAPP_HOST_TEST
static volatile uint64_t *const g_core_heartbeat =
    (volatile uint64_t *)(SDK_TASKQ_REGION_ADDRESS + ZZAPP_COHERENT_OFFSET);
#else
static volatile uint64_t g_core_heartbeat_word;
static volatile uint64_t *const g_core_heartbeat = &g_core_heartbeat_word;
static uint64_t g_test_ticks;
static uint32_t g_test_system_tick;
#endif

static zzapp_kernel_t g_kernel;
static int g_cold_restarts;
static int g_finish_tlb_on_stop = 1;

/* Host tests may inject work at the system-thread handoff point. */
#ifdef ZZAPP_HOST_TEST
static uint32_t g_test_service_calls;
static void (*g_test_service_hook)(void);
void scheduler_core1_service_from_app(void)
{
    g_test_service_calls++;
    if (g_test_service_hook)
        g_test_service_hook();
}
void zzapp_test_set_service_hook(void (*fn)(void)) { g_test_service_hook = fn; }
static int g_test_fail_start;
void zzapp_test_set_fail_start(int fail) { g_test_fail_start = fail; }
static void (*g_test_start_race_hook)(void);
void zzapp_test_set_start_race_hook(void (*fn)(void))
{
    g_test_start_race_hook = fn;
}
/* Runs once: core 0 may expire the claim between START's handoff and its
 * completion. */
static void test_start_race_point(void)
{
    void (*race_hook)(void) = g_test_start_race_hook;

    g_test_start_race_hook = NULL;
    if (race_hook)
        race_hook();
}
static void (*g_test_admission_hook)(void);
void zzapp_test_set_admission_hook(void (*fn)(void))
{
    g_test_admission_hook = fn;
}
/* Injection at a possible admission boundary: core 0 may claim the slot
 * here, and nothing core 1 writes afterwards may overwrite that claim. */
static void test_admission_point(void)
{
    if (g_test_admission_hook)
        g_test_admission_hook();
}
#else
static inline void test_start_race_point(void) {}
static inline void test_admission_point(void) {}
#endif

/* Per-record state lives in the process record. The hot paths take the
 * pointer arithmetic of zzapp_rec_by_pid(); an index outside the arena has
 * no record and stops the caller (hard_fail), it never aliases another. */
static zzapp_rec_t *rec_must(uint32_t pid)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

    if (!rec)
        hard_fail();
    return rec;
}

static zzapp_rec_coherent_t *coherent(uint32_t pid)
{
    return &rec_must(pid)->coh;
}

static zzapp_rec_host_t *host_slot(uint32_t pid)
{
    return &rec_must(pid)->host;
}

static zzapp_proc_t *proc_of(uint32_t pid)
{
    return &rec_must(pid)->proc;
}

#ifndef ZZAPP_HOST_TEST
static uint8_t *proc_alias(uint32_t offset, uint32_t bytes);
#endif

zzapp_proc_t *zzapp_proc_slot(uint32_t pid)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

    return rec ? &rec->proc : NULL;
}

zzapp_kernel_t *zzapp_runtime_kernel(void)
{
    return &g_kernel;
}

/* The selected context is restored onto the current exception stack, keeping
 * each exception mode's banked SP on the stack it owns. */
zzapp_arm_frame_t *zzapp_kernel_arch_dispatch(zzapp_arm_frame_t *frame,
                                               uint32_t source,
                                               uint32_t return_adjust)
{
    return zzapp_kernel_switch_frame(&g_kernel, frame,
        (zzapp_kernel_dispatch_t)source, return_adjust);
}

/* After an API call, service work takes precedence over returning to an app.
 * It runs only after the exception return path has selected the system thread. */
static int svc_current_runnable(void)
{
    int pid = zzapp_kernel_current_thread();
    zzapp_thread_t *thread = pid >= 0 ?
        zzapp_kernel_thread(&g_kernel, (uint32_t)pid) : NULL;

    return thread && thread->state == ZZAPP_THREAD_RUNNABLE &&
           !g_kernel.service_due && !thread->yield_pending;
}

zzapp_arm_frame_t *zzapp_kernel_svc_return(zzapp_arm_frame_t *frame)
{
    if (svc_current_runnable())
        return frame;
    return zzapp_kernel_arch_dispatch(frame, ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0U);
}

int zzapp_kernel_svc_can_return(void)
{
    return svc_current_runnable();
}

void zzapp_kernel_switch_address_space(uint32_t pid)
{
#ifndef ZZAPP_HOST_TEST
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

    if (rec)
        zzapp_as_switch(&rec->proc.as);
#else
    (void)pid;
#endif
}

void zzapp_kernel_switch_kernel_address_space(void)
{
#ifndef ZZAPP_HOST_TEST
    zzapp_as_switch_kernel();
#endif
}


/* The record the calling context works on (a pid). */
static uint32_t current_slot(void)
{
    return zzapp_services_current_pid();
}

static uint32_t select_teardown_slot(uint32_t pid)
{
    uint32_t selected = current_slot();

    zzapp_services_select_pid(pid);
    return selected;
}

static void restore_teardown_slot(uint32_t selected)
{
    zzapp_services_select_pid(selected);
}

int zzapp_proc_heap_grow(uint32_t bytes, void **old_break)
{
    uint32_t slot = current_slot();
    zzapp_proc_t *proc = proc_of(slot);

    if (!bytes || bytes % ZZAPP_MEM_FRAME_BYTES)
        return AAR_E_INVAL;
    if (bytes > proc->heap_limit - proc->heap_break ||
        bytes > proc->memory.limit - proc->memory.committed)
        return AAR_E_NOMEM;
    if (old_break)
#ifdef ZZAPP_HOST_TEST
        *old_break = host_slot(slot)->mem + proc->heap_break;
#else
        *old_break = (void *)(uintptr_t)(ZZAPP_AS_WINDOW_BASE + proc->heap_break);
#endif
#ifndef ZZAPP_HOST_TEST
    if (zzapp_loader_heap_grow(bytes) != AAR_OK)
        return AAR_E_NOMEM;
#endif
    proc->heap_break += bytes;
#ifdef ZZAPP_HOST_TEST
    proc->memory.committed += bytes;
#endif
    zzapp_xport_publish_private(rec_must(slot));
    return AAR_OK;
}

int zzapp_proc_heap_shrink(uint32_t bytes, void **new_break)
{
    uint32_t slot = current_slot();
    zzapp_proc_t *proc = proc_of(slot);

    if (!bytes || bytes % ZZAPP_MEM_FRAME_BYTES ||
        bytes > proc->heap_break - proc->heap_start)
        return AAR_E_INVAL;
#ifndef ZZAPP_HOST_TEST
    if (zzapp_loader_heap_shrink(bytes) != AAR_OK)
        return AAR_E_INVAL;
#endif
    proc->heap_break -= bytes;
#ifdef ZZAPP_HOST_TEST
    proc->memory.committed -= bytes;
#endif
    zzapp_xport_publish_private(rec_must(slot));
    if (new_break)
#ifdef ZZAPP_HOST_TEST
        *new_break = host_slot(slot)->mem + proc->heap_break;
#else
        *new_break = (void *)(uintptr_t)(ZZAPP_AS_WINDOW_BASE + proc->heap_break);
#endif
    return AAR_OK;
}

int zzapp_proc_heap_info(uint32_t *committed, uint32_t *limit,
                         uint32_t *pool_free)
{
    zzapp_proc_t *proc = proc_of(current_slot());

    if (!committed || !limit || !pool_free)
        return AAR_E_INVAL;
    *committed = proc->memory.committed;
    *limit = proc->memory.limit;
#ifndef ZZAPP_HOST_TEST
    *pool_free = zzapp_loader_pool_free();
#else
    *pool_free = 0u;
#endif
    return AAR_OK;
}

#ifdef ZZAPP_HOST_TEST
/* Stands in for the surface allocator's heap; the card's is about 78 MiB. */
static uint32_t g_test_host_visible_heap = 78u << 20;
#endif

uint32_t zzapp_host_visible_capacity(void)
{
    uint32_t alias = ZZAPP_AS_ALIAS_END - ZZAPP_AS_ALIAS_BASE;
#ifndef ZZAPP_HOST_TEST
    uint32_t heap = surface_allocator_total_bytes();
#else
    uint32_t heap = g_test_host_visible_heap;
#endif

    return heap < alias ? heap : alias;
}

/* The limit in force for a record: its launcher's request, else 16 MiB. */
static uint32_t host_visible_limit(uint32_t pid)
{
    uint32_t limit = zzapp_services_host_visible_limit(pid);

    return limit ? limit : ZZAPP_HOST_VISIBLE_DEFAULT_BYTES;
}
#define ZZAPP_HOST_VISIBLE_SLOTS ZZAPP_REC_VISIBLE_SLOTS
typedef zzapp_rec_visible_block_t zzapp_host_visible_block_t;

static zzapp_rec_visible_t *visible(uint32_t pid)
{
    return &rec_must(pid)->visible;
}
#ifndef ZZAPP_HOST_TEST
static int host_visible_alias_map(uint32_t slot, uint32_t card,
                                  uint32_t bytes, uint32_t align,
                                  uint32_t *alias)
{
    if (!alias || (card & 0xfffU) || !bytes || (bytes & 0xfffU) ||
        card + bytes < card || card + bytes > 0x40000000U)
        return -1;
    return zzapp_as_alias_map_aligned(&proc_of(slot)->as, card, bytes, 1,
                                      align, alias);
}

static int host_visible_alias_unmap(uint32_t slot, uint32_t alias,
                                    uint32_t bytes)
{
    return zzapp_as_alias_unmap(&proc_of(slot)->as, alias, bytes);
}
#endif


#ifdef ZZAPP_HOST_TEST
/* The tests' stand-in for the surface allocator. A block is identified by
 * its address and by the serial of the allocation that owns it, kept in the
 * record's alias field; the model reports a free whose serial no longer owns
 * the address, as the real allocator frees whoever holds it. */
extern uint32_t zzapp_test_card_alloc(uint32_t bytes, uint32_t align,
                                      uint32_t *serial) __attribute__((weak));
extern void zzapp_test_card_free(uint32_t card, uint32_t serial)
    __attribute__((weak));
#endif

/* Gives a record's resources back. The record is already detached: the
 * allocator frees by address and does not know the owner, so a record that
 * outlived its free could free a later owner's block. */
static void host_visible_retire(uint32_t slot,
                                const zzapp_host_visible_block_t *rec)
{
#ifndef ZZAPP_HOST_TEST
    /* A quarantined shell is released after its address space is gone, and
     * the alias went with it. */
    if (proc_of(slot)->as.active)
        (void)host_visible_alias_unmap(slot, rec->alias, rec->bytes);
    /* A non-cooperative reset can lose core 1's NULL detach store. This
     * rejects missing or differently sized blocks, not same-size reuse. */
    if (!g_finish_tlb_on_stop &&
        surface_allocator_block_size(rec->card) != rec->bytes) {
        printf("[app] recovery: skip stale shared block 0x%08lx (%lu bytes)\r\n",
               (unsigned long)rec->card, (unsigned long)rec->bytes);
        return;
    }
    (void)surface_allocator_free(rec->card);
#else
    (void)slot;
    if (zzapp_test_card_free)
        zzapp_test_card_free(rec->card, rec->alias);
    free(rec->ptr);
#endif
}

/* Teardown can be replayed after a core 1 restart that interrupted it. Each
 * block is therefore detached from its record before it is freed: a halt in
 * between leaks the block until the allocator is reset, where the other
 * order would let the replay free the address again after core 0 gave it to
 * someone else. */
static void host_visible_release_all(void)
{
    uint32_t slot = current_slot();
    zzapp_rec_visible_t *vis = visible(slot);
    zzapp_host_visible_block_t *blocks = vis->block;
    uint32_t i;

    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i) {
        zzapp_host_visible_block_t rec;

        if (!blocks[i].ptr)
            continue;
        rec = blocks[i];
        blocks[i].ptr = NULL;
        host_visible_retire(slot, &rec);
    }
    vis->used = 0;
    zzapp_xport_publish_shared(rec_must(slot));
}
void zzapp_host_revoke_visible(void)
{
    host_visible_release_all();
}


static void zzapp_private_timer_enable(void);
static void zzapp_private_timer_disable(void);

static void coherent_barrier(void)
{
#if defined(__arm__) && !defined(ZZAPP_HOST_TEST)
    __asm__ __volatile__("dmb sy" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

static void coherent_publish(void)
{
#if defined(__arm__) && !defined(ZZAPP_HOST_TEST)
    __asm__ __volatile__("dsb sy" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

static void wake_core1(void)
{
#if defined(__arm__) && !defined(ZZAPP_HOST_TEST)
    __asm__ __volatile__("sev" ::: "memory");
#endif
}

#ifdef ZZAPP_HOST_TEST
static void (*g_test_entry)(const void *api, int argc, char **argv);
static uint64_t (*g_test_time_hook)(void);
#else
static int g_private_timer_ready;
static int g_firmware_text_readonly;
extern char __firmware_text_start[];
extern char __firmware_text_end[];
#endif

static uint64_t now_ticks(void)
{
#ifdef ZZAPP_HOST_TEST
    if (g_test_time_hook)
        return g_test_time_hook();
    return g_test_ticks;
#else
    XTime now;
    XTime_GetTime(&now);
    return (uint64_t)now;
#endif
}

struct zzapp_u96 {
    uint32_t high;
    uint64_t low;
};

/* floor(2^64 * 1000000 / COUNTS_PER_SECOND), for the global timer's
 * COUNTS_PER_SECOND = CPU clock / 2.  The assertion makes a clock change
 * require a new reciprocal rather than silently skewing app time. */
#define ZZAPP_TICKS_PER_US_RECIPROCAL 55340230616261966ULL
#define ZZAPP_GLOBAL_TIMER_HZ 333333343U

static uint64_t mul_hi_u64(uint64_t a, uint64_t b)
{
    uint64_t a0 = (uint32_t)a;
    uint64_t a1 = a >> 32;
    uint64_t b0 = (uint32_t)b;
    uint64_t b1 = b >> 32;
    uint64_t p00 = a0 * b0;
    uint64_t p01 = a0 * b1;
    uint64_t p10 = a1 * b0;
    uint64_t p11 = a1 * b1;
    uint64_t middle = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;

    return p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
}

static struct zzapp_u96 mul_u64_u32(uint64_t value, uint32_t multiplier)
{
    uint64_t low_product = (uint32_t)value * (uint64_t)multiplier;
    uint64_t high_product = (value >> 32) * (uint64_t)multiplier +
                            (low_product >> 32);
    struct zzapp_u96 product;

    product.high = (uint32_t)(high_product >> 32);
    product.low = (high_product << 32) | (uint32_t)low_product;
    return product;
}

static int u96_le(struct zzapp_u96 a, struct zzapp_u96 b)
{
    return a.high < b.high || (a.high == b.high && a.low <= b.low);
}

uint64_t zzapp_ticks_to_us(uint64_t ticks)
{
    uint64_t us = mul_hi_u64(ticks, ZZAPP_TICKS_PER_US_RECIPROCAL);
    struct zzapp_u96 exact = mul_u64_u32(ticks, 1000000U);
    struct zzapp_u96 next = mul_u64_u32(us + 1U, ZZAPP_GLOBAL_TIMER_HZ);

    /*
     * The truncated reciprocal is low by less than ticks / 2^64, hence by
     * less than one for every uint64_t tick count.  `us` is therefore the
     * exact quotient or one below it; this 96-bit comparison corrects only
     * that latter case without a division.
     */
    if (u96_le(next, exact))
        ++us;
    return us;
}

static uint64_t ticks_to_us(uint64_t ticks)
{
#ifdef ZZAPP_HOST_TEST
    return ticks;
#else
    _Static_assert(COUNTS_PER_SECOND == ZZAPP_GLOBAL_TIMER_HZ,
                   "update ZZAPP_TICKS_PER_US_RECIPROCAL");
    return zzapp_ticks_to_us(ticks);
#endif
}

/* The selected record's control block, through the transport. A record
 * without a pair drops writes and reads 0. Words the platform writes go
 * through zzapp_xport_cb_put, words the client writes (stop request, lease
 * counter) through zzapp_xport_cb_read_client, which orders the load. */
static zzapp_rec_t *life_rec(void)
{
    return zzapp_rec_by_pid(zzapp_services_current_pid());
}

static void cb_put(uint32_t off, uint32_t value)
{
    zzapp_xport_cb_put(life_rec(), off, value);
}

static uint32_t cb_client(uint32_t off)
{
    return zzapp_xport_cb_read_client(life_rec(), off);
}

/* The selected record's state, as the loader and the worker see it. IDLE is
 * host-side only: the control block of a claim turns IDLE, with its end
 * reason, at the retire that ends the app (zzapp_services_retire_wire).
 * `end_reason` goes with the state word (AAR_END_*, 0 while there is none). */
static void publish_state_end(uint32_t state, uint32_t end_reason)
{
    uint32_t slot = current_slot();

    host_slot(slot)->state = state;
    coherent_publish();
    if (state != ZZAPP_ST_IDLE)
        zzapp_xport_cb_set_state(life_rec(), state, end_reason);
}

static void publish_state(uint32_t state)
{
    publish_state_end(state, ZZAPP_END_NONE);
}

void zzapp_host_register_low_memory_callback(zzapp_mem_t *memory)
{
    if (zzapp_mem_set_low_memory_callback)
        zzapp_mem_set_low_memory_callback(memory, zzapp_host_low_memory, NULL);
}

void zzapp_host_reset_low_memory(uint32_t pid)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

    if (!rec)
        return;
    __atomic_store_n(&rec->host.low_memory_pending, 0u, __ATOMIC_RELAXED);
}

/* Services admitted the record and published the claim (under its lock). */
void zzapp_host_claimed(uint32_t pid)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

    if (!rec)
        return;
    rec->host.terminal_reason = ZZAPP_END_NONE;
    (void)zzapp_rec_set_state(rec, ZZAPP_REC_LIVE);
}

void zzapp_loader_bound(uint32_t card, uint8_t *mem, uint32_t owner,
                        uint32_t generation)
{
    uint32_t slot = current_slot();
    zzapp_rec_host_t *host = host_slot(slot);
    zzapp_rec_coherent_t *coh = coherent(slot);

    host->card = card;
    host->mem = mem;
    host->owner = owner;
    host->terminal_reason = ZZAPP_END_NONE;
    host->stop_requested = 0u;
    host->stop_event_delivered = 0u;
    host->api_parked = 0u;
    visible(slot)->commit_active = 0u;
    zzapp_host_reset_low_memory(slot);
    proc_of(slot)->owner = owner;
    proc_of(slot)->generation = generation;
    coh->running = 0;
    coh->in_api = 0;
    coh->fault = ZZAPP_FAULT_NONE;
    coh->owner = owner;
    coh->generation = generation;
    /* The pair was initialised at admission (zeroed words, handle, queue
     * descriptors). A load starts from a clean fault record and exit code. */
    zzapp_xport_cb_set_fault(life_rec(), AAR_FAULT_NONE, 0u, 0u, 0u, 0u, 0u,
                             0u);
    cb_put(AAR_CB_EXIT_CODE, 0u);
}

/* Memory fell below the low-water mark: tell every running app. */
void zzapp_host_low_memory(void *context)
{
    uint32_t pid;
    uint32_t capacity = zzapp_rec_capacity();

    (void)context;
    for (pid = 0u; pid < capacity; pid++) {
        zzapp_rec_t *rec;
        zzapp_rec_host_t *host;

        if (zzapp_rec_state(pid) == ZZAPP_REC_FREE)
            continue;
        rec = zzapp_rec_by_pid(pid);
        host = &rec->host;
        if (!__atomic_load_n(&rec->coh.running, __ATOMIC_ACQUIRE))
            continue;
        __atomic_add_fetch(&host->low_memory_pending, 1u, __ATOMIC_RELAXED);
    }
}

int zzapp_host_take_low_memory_event(uint32_t pid)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);
    zzapp_rec_host_t *host;
    uint32_t pending;

    if (!rec)
        return 0;
    host = &rec->host;
    pending = __atomic_load_n(&host->low_memory_pending, __ATOMIC_ACQUIRE);
    while (pending != 0u) {
        if (__atomic_compare_exchange_n(&host->low_memory_pending, &pending,
                                        pending - 1u, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return 1;
    }
    return 0;
}

extern void zzapp_api_reclaim_resources(void) __attribute__((weak));
extern void zzapp_api_poison_resources(void) __attribute__((weak));
/* Stage 3 of a retire, host half: the record's host-visible blocks, the API
 * layer's resources, and the thread. A quarantined record (its client lost
 * the lease, so it may still write through a mapping) keeps its blocks: the
 * release of the shell returns them (zzapp_host_revoke_visible). */
void zzapp_loader_unbound_resources(void)
{
    uint32_t slot = current_slot();

    if (!zzapp_xport_flag_test(rec_must(slot), ZZAPP_XPORT_F_QUARANTINED))
        host_visible_release_all();
    if (zzapp_api_reclaim_resources)
        zzapp_api_reclaim_resources();
    zzapp_kernel_stop_app(&g_kernel, slot);
    visible(slot)->commit_active = 0u;
}

/* Stage 5 of a retire, host half: forget the host and process state. The
 * frames and the address space are already gone. */
void zzapp_loader_unbound(void)
{
    uint32_t slot = current_slot();
    zzapp_rec_t *rec = rec_must(slot);
    zzapp_rec_host_t *host = &rec->host;
    uint64_t fault_mark = host->fault_mark;
    int faulted = host->faulted;
    uint32_t exit_state = host->exit_state;

    /* The state is not cleared with the rest: services may have moved it
     * since this teardown began, and putting back a copy taken earlier would
     * undo that. */
    memset(host, 0, offsetof(zzapp_rec_host_t, state));
    memset((uint8_t *)host + offsetof(zzapp_rec_host_t, state) +
               sizeof(host->state),
           0,
           sizeof(*host) - offsetof(zzapp_rec_host_t, state) -
               sizeof(host->state));
    host->fault_mark = fault_mark;
    host->faulted = faulted;
    host->exit_state = exit_state;
    memset(&rec->proc, 0, sizeof(rec->proc));
    zzapp_xport_publish_private(rec);
    rec->coh.owner = 0;
}

/* The accessors below answer for the selected record; without one the
 * answer is the idle one, and nothing is written. */
static zzapp_rec_t *selected_rec(void)
{
    return zzapp_rec_by_pid(current_slot());
}

uint32_t zzapp_loader_generation(void)
{
    zzapp_rec_t *rec = selected_rec();

    return rec ? rec->coh.generation : 0u;
}

uint32_t zzapp_loader_state(void)
{
    zzapp_rec_t *rec = selected_rec();

    return rec ? rec->host.state : ZZAPP_ST_IDLE;
}

void zzapp_loader_set_state(uint32_t state) { publish_state(state); }

uint32_t zzapp_loader_reset_requested(void)
{
    zzapp_rec_t *rec = selected_rec();

    return rec ? rec->coh.reset_request : 0u;
}

int zzapp_loader_have_image(void)
{
    zzapp_rec_t *rec = selected_rec();

    return rec ? rec->host.image_loaded : 0;
}

int zzapp_loader_set_image(uint32_t entry, uint32_t image_end,
                           uint32_t stack_bytes, uint32_t note_flags,
                           uint32_t min_heap_bytes)
{
    uint32_t slot = current_slot();
    zzapp_rec_host_t *host = host_slot(slot);
    zzapp_proc_t *proc = proc_of(slot);
    uint32_t image_bytes = (image_end + ZZAPP_MEM_FRAME_BYTES - 1u) &
                           ~(ZZAPP_MEM_FRAME_BYTES - 1u);
    uint32_t stack_commit = (stack_bytes + ZZAPP_MEM_FRAME_BYTES - 1u) &
                            ~(ZZAPP_MEM_FRAME_BYTES - 1u);
    uint32_t commit = image_bytes + stack_commit + min_heap_bytes;
    uint32_t limit = ZZAPP_MEM_PRIVATE_WINDOW_BYTES;
    uint32_t requested_limit = zzapp_services_memory_limit();

    if (requested_limit != 0u && requested_limit < limit)
        limit = requested_limit;
    if (commit > limit)
        return AAR_E_NOMEM;
    host->entry = entry;
    host->image_loaded = 1;
    host->image_end = image_end;
    host->stack_bytes = stack_bytes;
    proc->image_entry = entry;
    proc->image_end = image_end;
    proc->image_frame_count = image_bytes / ZZAPP_MEM_FRAME_BYTES;
    proc->stack_frame_count = stack_commit / ZZAPP_MEM_FRAME_BYTES;
    proc->stack_bytes = stack_bytes;
#ifdef ZZAPP_HOST_TEST
    proc->heap_base = image_bytes;
    proc->stack_base = ZZAPP_MEM_PRIVATE_WINDOW_BYTES - stack_commit;
    proc->heap_limit = ZZAPP_AS_ALIAS_BASE - ZZAPP_AS_WINDOW_BASE;
    proc->heap_frame_count = min_heap_bytes / ZZAPP_MEM_FRAME_BYTES;
#endif
    proc->memory.committed = commit;
    proc->memory.limit = limit;
    zzapp_xport_publish_private(rec_must(slot));
    cb_put(AAR_CB_FLAGS, note_flags & (AAR_CBF_DISPLAY | AAR_CBF_AUDIO |
                                        AAR_CBF_GL));
    return AAR_OK;
}

uint32_t zzapp_current_generation(void) { return zzapp_loader_generation(); }
uint32_t zzapp_runtime_state(void) { return zzapp_loader_state(); }
uint32_t zzapp_runtime_caps(void) { return ZZAPP_CAP_Z3 | ZZAPP_CAP_DISPLAY | ZZAPP_CAP_AUDIO | ZZAPP_CAP_GL | ZZAPP_CAP_CLUT8; }

#ifdef ZZAPP_HOST_TEST
static uint32_t g_test_groups;
static int g_test_groups_override;
#endif

/* Input has no table slots: the client's input events feed event_poll
 * whenever the runtime serves a client, which every ZZ9000 runtime does. */
uint32_t zzapp_runtime_groups(void)
{
    uint32_t caps = zzapp_runtime_caps();
    uint32_t groups = AAR_GROUP_INPUT | AAR_GROUP_SERVICES;

#ifdef ZZAPP_HOST_TEST
    if (g_test_groups_override)
        return g_test_groups;
#endif
    if (caps & ZZAPP_CAP_DISPLAY)
        groups |= AAR_GROUP_DISPLAY;
    if (caps & ZZAPP_CAP_AUDIO)
        groups |= AAR_GROUP_AUDIO;
    if (caps & ZZAPP_CAP_GL)
        groups |= AAR_GROUP_GL;
    return groups;
}
int zzapp_host_faulted(void)
{
    zzapp_rec_t *rec = selected_rec();

    return rec ? rec->host.faulted : 0;
}

void zzapp_note_progress(void)
{
    zzapp_rec_t *rec = selected_rec();
    uint64_t now = now_ticks();

    if (!rec)
        return;
    rec->coh.progress = now;
}

void zzapp_host_latch_stop(uint32_t pid)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

    if (rec)
        rec->host.stop_requested = 1u;
}

void zzapp_host_clear_stop_requested(uint32_t pid)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

    if (rec) {
        rec->host.stop_requested = 0u;
        rec->host.stop_event_delivered = 0u;
    }
}

int zzapp_host_stop_requested(void)
{
    zzapp_rec_t *rec = selected_rec();

    return rec ? rec->host.stop_requested != 0u : 0;
}

int zzapp_host_take_stop_event(uint32_t pid)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);
    zzapp_rec_host_t *host;

    if (!rec)
        return 0;
    host = &rec->host;
    if (!__atomic_load_n(&host->stop_requested, __ATOMIC_ACQUIRE))
        return 0;
    return __atomic_exchange_n(&host->stop_event_delivered, 1u,
                               __ATOMIC_ACQ_REL) == 0u;
}

int zzapp_host_note_stop_event_delivered(uint32_t pid)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

    if (!rec)
        return 1;
    return __atomic_exchange_n(&rec->host.stop_event_delivered, 1u,
                               __ATOMIC_ACQ_REL) != 0u;
}

static int stop_requested(void)
{
    return zzapp_host_stop_requested();
}

/* Core 1 (service tick, faults) and core 0 (restart recovery) can both stop
 * the app; exactly one of them runs the teardown. */
static int claim_fault(uint32_t slot)
{
    return __atomic_exchange_n(&host_slot(slot)->faulted, 1, __ATOMIC_ACQ_REL) == 0;
}
static uint32_t terminal_reason(const zzapp_rec_host_t *host,
                                uint32_t fault)
{
    if (host->terminal_reason)
        return host->terminal_reason;
    if (fault == ZZAPP_FAULT_DATA || fault == ZZAPP_FAULT_PREFETCH ||
        fault == ZZAPP_FAULT_UNDEF)
        return ZZAPP_END_USER_FAULT;
    if (fault == ZZAPP_FAULT_WATCHDOG)
        return host->watchdog_reason == 2u ? ZZAPP_END_STUCK_CALL :
                                              ZZAPP_END_STALL;
    if (fault == ZZAPP_FAULT_WATCHDOG_RESTART)
        return ZZAPP_END_CORE_RESTART;
    return ZZAPP_END_FORCED;
}


static void snapshot_stop_diagnostics(uint32_t slot)
{
    zzapp_rec_host_t *host = host_slot(slot);
    zzapp_rec_coherent_t *coh = coherent(slot);

    if (host->stop_diagnostics_valid)
        return;
    host->stop_thread_state = (uint32_t)zzapp_kernel_thread(&g_kernel, slot)->state;
    host->stop_wake_reason =
        (uint32_t)zzapp_kernel_thread(&g_kernel, slot)->wake_reason;
    host->stop_in_api = coh->in_api;
    host->stop_parked = host->api_parked;
    host->stop_wait_registered =
        (uint32_t)zzapp_services_wait_registered(slot);
#ifndef ZZAPP_HOST_TEST
    host->stop_svc_number = zzapp_svc_last_number_for(slot);
#else
    host->stop_svc_number = 0u;
#endif
    host->stop_diagnostics_valid = 1u;
}

static void request_stop(uint32_t slot, uint32_t fault, uint32_t pc,
                         uint32_t sp, uint32_t lr, uint32_t far,
                         uint32_t fsr, uint32_t cpsr)
{
    zzapp_rec_host_t *host = host_slot(slot);
    zzapp_rec_coherent_t *coh = coherent(slot);
    uint32_t expected = 0u;
    zzapp_host_latch_stop(slot);

    if (!__atomic_compare_exchange_n(&host->stop_pending, &expected, 2u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    host->pending_fault = fault;
    host->pending_pc = pc;
    host->pending_sp = sp;
    host->pending_lr = lr;
    host->pending_far = far;
    host->pending_fsr = fsr;
    host->pending_cpsr = cpsr;
    snapshot_stop_diagnostics(slot);
    coh->running = 0u;
    zzapp_kernel_stop_app(&g_kernel, slot);
    zzapp_kernel_service_due(&g_kernel);
    __atomic_store_n(&host->stop_pending, 1u, __ATOMIC_RELEASE);
}

static void force_stop_slot(uint32_t slot, uint32_t fault, uint32_t pc,
                            uint32_t sp, uint32_t lr, uint32_t far,
                            uint32_t fsr, uint32_t cpsr, int finish_tlb)
{
#if !ZZAPP_PERF_COUNTERS || defined(ZZAPP_HOST_TEST)
    (void)finish_tlb;
#endif
    zzapp_rec_host_t *host = host_slot(slot);
    zzapp_rec_coherent_t *coh = coherent(slot);
    uint32_t selected;

    (void)cpsr;
    if (!claim_fault(slot))
        return;
    selected = select_teardown_slot(slot);
    coh->running = 0;
    coh->fault = fault;
    zzapp_xport_cb_set_fault(life_rec(), zzapp_fault_to_aar(fault, fsr), fsr,
                             cpsr, pc, sp, lr, far);
    snapshot_stop_diagnostics(slot);
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    zzapp_svc_counters_report(slot);
    if (finish_tlb)
        zzapp_kernel_perf_finish_slot(&g_kernel, slot);
    zzapp_kernel_perf_request_report(&g_kernel, slot, 1);
#endif
    zzapp_kernel_stop_app(&g_kernel, slot);
#ifndef ZZAPP_HOST_TEST
    {
        const uint32_t *stk = (const uint32_t *)(uintptr_t)sp;
        uint32_t w[6];
        uint32_t i;
        int readable = 1;

        for (i = 0U; i < 6U; ++i) {
            w[i] = 0U;
            if (readable && (uintptr_t)(stk + i) >= 0x0b000000U &&
                (uintptr_t)(stk + i) < 0x11000000U)
                w[i] = stk[i];
            else
                readable = 0;
        }
        printf("[app] fault stack: %08lx %08lx %08lx %08lx %08lx %08lx (sp=%08lx)\r\n",
               (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2],
               (unsigned long)w[3], (unsigned long)w[4], (unsigned long)w[5],
               (unsigned long)sp);
        printf("[app] stop slot=%lu fault=%lu reason=%lu pc=%08lx lr=%08lx "
               "far=%08lx fsr=%08lx thread=%lu wake=%lu in_api=%lu "
               "svc=%lu parked=%lu wait=%d\r\n",
               (unsigned long)slot, (unsigned long)fault,
               (unsigned long)terminal_reason(host, fault),
               (unsigned long)pc, (unsigned long)lr, (unsigned long)far,
               (unsigned long)fsr,
               (unsigned long)host->stop_thread_state,
               (unsigned long)host->stop_wake_reason,
               (unsigned long)host->stop_in_api,
               (unsigned long)host->stop_svc_number,
               (unsigned long)host->stop_parked,
               (int)host->stop_wait_registered);
        fflush(stdout);
    }
#endif
    publish_state_end(ZZAPP_ST_FAULTED, terminal_reason(host, fault));
    host->fault_mark = now_ticks();
    zzapp_services_set_terminal(terminal_reason(host, fault));
    zzapp_services_on_app_end();
    zzapp_minigl_reclaim(coh->generation);
    if (zzapp_api_poison_resources)
        zzapp_api_poison_resources();
    (void)zzapp_rec_retire(slot);
    restore_teardown_slot(selected);
}

static void force_stop(uint32_t fault, uint32_t pc, uint32_t sp,
                       uint32_t lr, uint32_t far, uint32_t fsr, uint32_t cpsr)
{
    force_stop_slot(current_slot(), fault, pc, sp, lr, far, fsr, cpsr,
                    g_finish_tlb_on_stop);
}

static void complete_pending_stop(uint32_t slot)
{
    zzapp_rec_host_t *host = host_slot(slot);
    uint32_t expected = 1u;

    if (!__atomic_compare_exchange_n(&host->stop_pending, &expected, 0u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    force_stop_slot(slot, host->pending_fault, host->pending_pc,
                    host->pending_sp, host->pending_lr, host->pending_far,
                    host->pending_fsr, host->pending_cpsr, 1);
}

/* A normal exit's teardown is owed from the exit call until it has finished,
 * so core 0 can tell an exited app whose resources are still live from an
 * idle record. The host block cannot hold it past the arena drop (it is the
 * one field the unbind keeps). */
#define ZZAPP_EXIT_NONE      0u
#define ZZAPP_EXIT_REQUESTED 1u
#define ZZAPP_EXIT_RUNNING   2u

static int exit_busy(uint32_t pid)
{
    return __atomic_load_n(&host_slot(pid)->exit_state, __ATOMIC_ACQUIRE) !=
           ZZAPP_EXIT_NONE;
}

/* Whether core 1 still owes the record teardown: the app runs, or it exited
 * and its cleanup has not finished. Running is loaded first with acquire,
 * see request_exit. */
static int slot_owes_teardown(uint32_t slot)
{
    if (__atomic_load_n(&coherent(slot)->running, __ATOMIC_ACQUIRE))
        return 1;
    return exit_busy(slot);
}

/* The app thread's side of a normal exit: record it, stop the thread and
 * leave. Nothing that can take long runs here, because a stopped thread is
 * never scheduled again: a tick that switched away between stopping it and
 * recording the exit would strand the rest, so the transition is one IRQ
 * masked step. The exit is recorded before running reads zero, and running is
 * published with release semantics, so whoever sees the app stopped also sees
 * the teardown it owes. */
static void request_exit(uint32_t slot)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(slot);
    uint32_t irq;

    if (!rec)
        return;
    irq = smp_local_irq_save();

    __atomic_store_n(&rec->host.exit_state, ZZAPP_EXIT_REQUESTED,
                     __ATOMIC_RELEASE);
    /* Cross-core order: core 0 reads running, then exit_state. A release
     * store of running after the exit state, paired with an acquire load of
     * running before it, makes "stopped" imply "exit owed" there. Host tests
     * cannot show memory ordering; this is by construction. The IRQ mask
     * only orders core 1 against its own scheduler. */
    __atomic_store_n(&rec->coh.running, 0u, __ATOMIC_RELEASE);
    zzapp_kernel_stop_app(&g_kernel, slot);
    zzapp_kernel_service_due(&g_kernel);
    smp_local_irq_restore(irq);
}

/* The system thread's side: the teardown that frees the arena, unmapping
 * every host-visible block. The scheduler resumes the system thread after
 * an interrupt and other apps keep running meanwhile. The exit stays owed
 * until the last step is done. */
static void complete_pending_exit(uint32_t slot)
{
    zzapp_rec_host_t *host = host_slot(slot);
    zzapp_rec_coherent_t *coh = coherent(slot);
    uint32_t expected = ZZAPP_EXIT_REQUESTED;
    uint32_t selected;

    if (!__atomic_compare_exchange_n(&host->exit_state, &expected,
                                     ZZAPP_EXIT_RUNNING, 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;
    /* A fault stop that won the race has torn the record down already. */
    if (host->faulted) {
        __atomic_store_n(&host->exit_state, ZZAPP_EXIT_NONE,
                         __ATOMIC_RELEASE);
        return;
    }
    selected = select_teardown_slot(slot);
    host->watchdog_reason = 0u;
    if (coh->reset_request) {
        zzapp_services_set_terminal(ZZAPP_END_AMIGA_RESET);
    } else {
        cb_put(AAR_CB_EXIT_CODE, host->exit_code);
        zzapp_services_set_terminal(host->terminal_reason ?
                                    host->terminal_reason : ZZAPP_END_EXIT);
    }
    zzapp_services_on_app_end();
    zzapp_minigl_reclaim(coh->generation);
    publish_state(ZZAPP_ST_IDLE);
    /* The exit stays owed until the record is gone: a core-1 restart that
     * interrupts the retire finds it and replays it. */
    (void)zzapp_rec_retire(slot);
    __atomic_store_n(&host->exit_state, ZZAPP_EXIT_NONE, __ATOMIC_RELEASE);
    restore_teardown_slot(selected);
}


#ifdef ZZAPP_HOST_TEST
/* Stop zzapp_rec_retire() after the stage with this number (zzapp_retire_stage_t,
 * 1 DETACHED .. 5 WIRE), as a core-1 halt would. Also stops it after a halt the
 * record layer simulated inside its own stage (zzapp_rec_test_halt_step). */
unsigned zzapp_test_retire_halt_stage;
#define RETIRE_HALT(stage) \
    do { \
        if (zzapp_test_retire_halt_stage == (unsigned)(stage)) { \
            restore_teardown_slot(selected); \
            return 0; \
        } \
    } while (0)
#else
#define RETIRE_HALT(stage) do { } while (0)
#endif

/*
 * The single teardown path of a record (design 4.2). Each stage is idempotent
 * and marks its number (zzapp_retire_stage_t) in the record, so a core-1
 * restart that interrupted it replays the rest:
 *   DETACHED   state RETIRING, the thread leaves the run list for good
 *   RESOURCES  host-visible blocks, API resources, loader staging and back
 *              buffer (zzapp_loader_retire_resources)
 *   TABLES     alias extents, L2 list, L2 reserve, L1, ASID (zzapp_rec.c)
 *   COMMIT     frames returned to the ledger, host and process state cleared
 *   WIRE       services publishes IDLE, then frees the record or holds the
 *              shell and its transport until the client releases it
 */
int zzapp_rec_retire(uint32_t pid)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);
    uint32_t selected;
    int freed;

    if (!rec)
        return -1;
    if (zzapp_rec_state(pid) == ZZAPP_REC_FREE)
        return 1;
    selected = select_teardown_slot(pid);
    /* A state that is already RETIRING or RECLAIMED stays: the transition only
     * moves forward. */
    (void)zzapp_rec_set_state(rec, ZZAPP_REC_RETIRING);
    zzapp_kernel_stop_app(&g_kernel, pid);
    zzapp_rec_set_stage(rec, ZZAPP_RETIRE_DETACHED);
    RETIRE_HALT(ZZAPP_RETIRE_DETACHED);
    zzapp_loader_retire_resources();
    zzapp_rec_set_stage(rec, ZZAPP_RETIRE_RESOURCES);
    RETIRE_HALT(ZZAPP_RETIRE_RESOURCES);
    zzapp_rec_retire_tables(rec);
#ifdef ZZAPP_HOST_TEST
    if (zzapp_rec_test_halt_step != 0U) {
        restore_teardown_slot(selected);
        return 0;
    }
#endif
    RETIRE_HALT(ZZAPP_RETIRE_TABLES);
    zzapp_loader_retire_commit();
    /* The host's own view of the record is idle once its resources are gone;
     * a fault keeps its state, which STATUS reads until it is observed. */
    if (rec->host.state != ZZAPP_ST_FAULTED)
        rec->host.state = ZZAPP_ST_IDLE;
    zzapp_rec_set_stage(rec, ZZAPP_RETIRE_COMMIT);
    RETIRE_HALT(ZZAPP_RETIRE_COMMIT);
    freed = zzapp_services_retire_wire(rec);
    if (!freed)
        (void)zzapp_rec_set_state(rec, ZZAPP_REC_RECLAIMED);
    restore_teardown_slot(selected);
    return freed ? 1 : 0;
}

static void maybe_expire_fault(uint64_t now)
{
    zzapp_rec_host_t *host = host_slot(current_slot());

    if (host->state == ZZAPP_ST_FAULTED && now > host->fault_mark &&
        ticks_to_us(now - host->fault_mark) >= ZZAPP_WATCHDOG_US)
        publish_state(ZZAPP_ST_IDLE);
}

static void begin_tick_observation(void)
{
    uint32_t slot = current_slot();
    zzapp_rec_coherent_t *coh = coherent(slot);
    zzapp_rec_host_t *host = host_slot(slot);

    host->in_api = 0u;
    host->api_parked = 0u;
    host->service_due = 0u;
    host->seen_api_calls = host->api_calls;
    host->last_tick = now_ticks();
    coh->in_api = 0u;
    *g_core_heartbeat = host->last_tick;
}

static void check_stack_canary(void);

/* The private timer may interrupt an app API or system service pass. It only
 * updates scheduler-visible state; teardown and service code stay on the
 * system thread. */
static void service_tick_irq(uint64_t now)
{
    uint32_t slot;
    zzapp_rec_host_t *host;
    zzapp_rec_coherent_t *coh;
    uint32_t calls;

    *g_core_heartbeat = now;
    zzapp_kernel_service_due(&g_kernel);
    slot = (uint32_t)zzapp_kernel_current_thread();
#ifdef ZZAPP_HOST_TEST
    if (!zzapp_rec_by_pid(slot)) {
        if (g_test_system_tick != 0U)
            return;
        slot = current_slot();
    }
#endif
    if (!zzapp_rec_by_pid(slot))
        return;
    host = host_slot(slot);
    coh = coherent(slot);
    coh->tick_seen = now;
    calls = host->api_calls;
    coh->in_api = host->in_api;
    if (calls != host->seen_api_calls) {
        host->seen_api_calls = calls;
        coh->progress = now;
    }
    host->last_tick = now;
    /* The client can set this host-visible word before its best-effort STOP
     * reaches firmware. Sample it at the scheduler tick, never per SVC. */
    if (cb_client(AAR_CB_STOP_REQUEST) != 0u)
        zzapp_host_latch_stop(slot);
    if (visible(slot)->commit_active || !coh->running)
        goto finish;
    if (coh->reset_request || host->stop_requested != 0u) {
        if (!host->stop_mark)
            host->stop_mark = now;
        if (coh->reset_request ||
            ticks_to_us(now - host->stop_mark) >= ZZAPP_STOP_GRACE_US)
            request_stop(slot, ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
    } else if (host->in_api == 0u && !host->api_parked &&
               ticks_to_us(now - coh->progress) >= ZZAPP_APP_STALL_US) {
        host->watchdog_reason = 1u;
        request_stop(slot, ZZAPP_FAULT_WATCHDOG, 0, 0, 0, 0, 0, 0);
    } else if (host->in_api != 0u && !host->api_parked &&
               ticks_to_us(now - host->api_entered) >= ZZAPP_API_STUCK_US) {
        host->watchdog_reason = 2u;
        request_stop(slot, ZZAPP_FAULT_WATCHDOG, 0, 0, 0, 0, 0, 0);
    }

finish:
    host->service_due = 1u;
}



static void service_watch_slot(uint64_t now)
{
    uint32_t slot = current_slot();
    zzapp_rec_host_t *host = host_slot(slot);
    zzapp_rec_coherent_t *coh = coherent(slot);
    check_stack_canary();

    /* The worker visits every slot on each system pass. The client can write
     * this host-visible word even when its best-effort STOP is dropped, so
     * sample every valid slot rather than only the timer's interrupted app. */
    if (cb_client(AAR_CB_STOP_REQUEST) != 0u)
        zzapp_host_latch_stop(slot);
    if (visible(slot)->commit_active)
        return;
    if (coh->running && stop_requested()) {
        if (!host->stop_mark)
            host->stop_mark = now;
        if (coh->reset_request ||
            ticks_to_us(now - host->stop_mark) >= ZZAPP_STOP_GRACE_US)
            request_stop(slot, ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
    } else {
        host->stop_mark = 0;
    }
    maybe_expire_fault(now);
}

static void check_stack_canary(void);


#ifndef ZZAPP_HOST_TEST
static void zzapp_private_timer_irq(void *unused)
{
    uint64_t now;

    (void)unused;
    Xil_Out32(XPAR_PS7_SCUTIMER_0_BASEADDR + XSCUTIMER_ISR_OFFSET,
              XSCUTIMER_ISR_EVENT_FLAG_MASK);
    now = now_ticks();
    service_tick_irq(now);
}

static void zzapp_private_timer_disable(void)
{
    if (!g_private_timer_ready)
        return;
    Xil_Out32(XPAR_PS7_SCUTIMER_0_BASEADDR + XSCUTIMER_CONTROL_OFFSET, 0u);
    XScuGic_Disable(interrupt_get_intc(), XPAR_PS7_SCUTIMER_0_INTR);
}

static void zzapp_private_timer_enable(void)
{
    if (!g_private_timer_ready)
        return;
    Xil_Out32(XPAR_PS7_SCUTIMER_0_BASEADDR + XSCUTIMER_ISR_OFFSET,
              XSCUTIMER_ISR_EVENT_FLAG_MASK);
    XScuGic_Enable(interrupt_get_intc(), XPAR_PS7_SCUTIMER_0_INTR);
    Xil_Out32(XPAR_PS7_SCUTIMER_0_BASEADDR + XSCUTIMER_CONTROL_OFFSET,
              XSCUTIMER_CONTROL_IRQ_ENABLE_MASK |
              XSCUTIMER_CONTROL_AUTO_RELOAD_MASK |
              XSCUTIMER_CONTROL_ENABLE_MASK);
}

/* Doorbell SGI while an app runs: let its service tick fire on the next
 * timer count instead of at the end of the 5 ms quantum. The timer then
 * reloads its normal period. */
void zzapp_private_timer_kick(void)
{
    if (!g_private_timer_ready ||
        !(Xil_In32(XPAR_PS7_SCUTIMER_0_BASEADDR + XSCUTIMER_CONTROL_OFFSET) &
          XSCUTIMER_CONTROL_ENABLE_MASK))
        return;
    Xil_Out32(XPAR_PS7_SCUTIMER_0_BASEADDR + XSCUTIMER_COUNTER_OFFSET, 1u);
}

void zzapp_private_timer_init_core1(void)
{
    XScuGic *intc = interrupt_get_intc();

    Xil_Out32(XPAR_PS7_SCUTIMER_0_BASEADDR + XSCUTIMER_CONTROL_OFFSET, 0u);
    Xil_Out32(XPAR_PS7_SCUTIMER_0_BASEADDR + XSCUTIMER_LOAD_OFFSET,
              (uint32_t)(COUNTS_PER_SECOND / (1000000u / ZZAPP_TICK_US)));
    Xil_Out32(XPAR_PS7_SCUTIMER_0_BASEADDR + XSCUTIMER_ISR_OFFSET,
              XSCUTIMER_ISR_EVENT_FLAG_MASK);
    if (XScuGic_Connect(intc, XPAR_PS7_SCUTIMER_0_INTR,
                        (Xil_InterruptHandler)zzapp_private_timer_irq, NULL)
        != XST_SUCCESS)
        return;
    g_private_timer_ready = 1;
    zzapp_private_timer_disable();
    /* The distributor is shared, but its CPU interface and PPIs are banked.
     * This is the same local setup XScuGic_CfgInitialize performs on core 0. */
    XScuGic_CPUWriteReg(intc, XSCUGIC_CPU_PRIOR_OFFSET, 0xf0u);
    XScuGic_CPUWriteReg(intc, XSCUGIC_CONTROL_OFFSET, 0x07u);
    /* PPI 29 is enabled only on this core, and only for a running app. */
    Xil_ExceptionEnable();
}
#else
void zzapp_private_timer_init_core1(void) {}
static void zzapp_private_timer_enable(void) {}
static void zzapp_private_timer_disable(void) {}
void zzapp_private_timer_kick(void) {}
#endif



/* The 64-byte canary below the app stack is set in run_app. Docs promise a
 * force-stop with ZZAPP_FAULT_DATA when it is damaged; check it on every
 * service pass. */
static void check_stack_canary(void)
{
    zzapp_rec_host_t *host = host_slot(current_slot());
    uint32_t i;

    if (!host->mem || host->stack_canary == 0u ||
        !coherent(current_slot())->running)
        return;
#ifdef ZZAPP_HOST_TEST
    for (i = 0; i < ZZAPP_CANARY_BYTES; i++) {
        if (host->mem[host->stack_canary + i] != 0xA5u) {
            request_stop(current_slot(), ZZAPP_FAULT_DATA, 0, 0, 0, 0, 0, 0);
            return;
        }
    }
#else
    {
        uint8_t *canary = proc_alias(host->stack_canary, ZZAPP_CANARY_BYTES);

        if (!canary)
            return;
        for (i = 0; i < ZZAPP_CANARY_BYTES; i++)
            if (canary[i] != 0xA5u) {
                request_stop(current_slot(), ZZAPP_FAULT_DATA, 0, 0, 0, 0, 0, 0);
                return;
            }
    }
#endif
}
#ifndef ZZAPP_HOST_TEST
static uint32_t api_lock_irq_save(void)
{
    uint32_t cpsr;

    __asm__ __volatile__("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) :: "memory");
    return cpsr;
}

static void api_lock_irq_restore(uint32_t cpsr)
{
    if ((cpsr & 0x80u) == 0u)
        __asm__ __volatile__("cpsie i" ::: "memory");
}
#endif

static int api_lock_until_acquired(uint32_t slot)
{
    int contended = 0;

    /*
     * A timer can interrupt immediately after the scheduler wakes this
     * waiter, before zzapp_api_enter publishes its API call.  Keep the
     * normal stall watchdog out of that handoff window.
     */
    host_slot(slot)->api_parked = 1u;
    while (1) {
#ifndef ZZAPP_HOST_TEST
        uint32_t cpsr = api_lock_irq_save();
#endif

        if (zzapp_kernel_api_lock(&g_kernel, slot) == 0) {
#ifndef ZZAPP_HOST_TEST
            api_lock_irq_restore(cpsr);
#endif
            zzapp_note_progress();
            return contended;
        }
        contended = 1;
        (void)zzapp_kernel_block(&g_kernel, slot);
#ifndef ZZAPP_HOST_TEST
        api_lock_irq_restore(cpsr);
        __asm__ __volatile__("svc 0" ::: "memory");
#else
        extern void zzapp_test_api_wait_hook(void) __attribute__((weak));

        if (!zzapp_test_api_wait_hook)
            abort();
        zzapp_test_api_wait_hook();
#endif
        zzapp_note_progress();
    }
}

static void resume_parked_api(uint32_t slot)
{
    zzapp_rec_host_t *host = host_slot(slot);
    uint64_t now = now_ticks();

    host->api_entered = now;
    host->seen_api_calls = host->api_calls;
    coherent(slot)->progress = now;
    host->api_parked = 0u;
}


void zzapp_host_yield(void)
{
    uint32_t slot = current_slot();
    zzapp_rec_t *rec = zzapp_rec_by_pid(slot);
    zzapp_rec_host_t *host;

    /* A system context with no record selected has nothing to yield for:
     * only ask for a service pass. */
    if (!rec) {
        zzapp_kernel_service_due(&g_kernel);
        return;
    }
    host = &rec->host;

    zzapp_note_progress();
    if (zzapp_kernel_current_thread() >= 0 && host->in_api == 0u)
        check_stack_canary();
    /* Yield is the API body's safe interleaving point. Never preempt a
     * non-yielding body: it may hold service-visible state on its stack. */
    if (zzapp_kernel_current_thread() < 0 || host->in_api == 0u ||
        g_kernel.api_lock_owner != (int)slot ||
        g_kernel.api_lock_depth != 1U) {
        if (zzapp_kernel_current_thread() < 0) {
            host->service_due = 1u;
            zzapp_kernel_service_due(&g_kernel);
        } else {
            zzapp_kernel_yield(&g_kernel);
        }
        return;
    }
    host->service_due = 1u;
    zzapp_kernel_service_due(&g_kernel);
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    if (host->perf_svc_sampled)
        zzapp_kernel_perf_service_end(&g_kernel, ZZAPP_PERF_SERVICE_SVC,
                                      scheduler_perf_stamp());
#endif
    host->api_parked = 1u;
    (void)zzapp_kernel_api_unlock(&g_kernel, slot);
#ifndef ZZAPP_HOST_TEST
    __asm__ __volatile__("svc 0" ::: "memory");
#else
    {
        extern void zzapp_test_api_yield_hook(void) __attribute__((weak));

        if (zzapp_test_api_yield_hook)
            zzapp_test_api_yield_hook();
    }
#endif
    (void)api_lock_until_acquired(slot);
    resume_parked_api(slot);
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    if (host->perf_svc_sampled)
        zzapp_kernel_perf_service_begin(&g_kernel, ZZAPP_PERF_SERVICE_SVC,
                                        scheduler_perf_stamp());
#endif
}

void zzapp_host_yield_wait(void)
{
    uint32_t slot = current_slot();
    zzapp_rec_t *rec = zzapp_rec_by_pid(slot);
    zzapp_rec_host_t *host;

    if (!rec) {
        zzapp_kernel_service_due(&g_kernel);
        return;
    }
    host = &rec->host;

    if (zzapp_kernel_current_thread() < 0) {
        zzapp_note_progress();
        host->service_due = 1u;
        zzapp_kernel_service_due(&g_kernel);
        return;
    }
    if (host->in_api == 0u) {
        zzapp_host_yield();
        return;
    }
    host->api_parked = zzapp_services_wait_registered(slot) ? 1u : 0u;
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    if (g_kernel.api_lock_depth == 1U && host->perf_svc_sampled)
        zzapp_kernel_perf_service_end(&g_kernel, ZZAPP_PERF_SERVICE_SVC,
                                      scheduler_perf_stamp());
#endif
    if (!host->api_parked) {
        (void)zzapp_kernel_api_sleep(&g_kernel, slot, 1U);
    } else {
        (void)zzapp_kernel_api_block(&g_kernel, slot);
    }
    zzapp_note_progress();
#ifndef ZZAPP_HOST_TEST
    /*
     * The core-1 SVC vector recognises a privileged SVC as a voluntary
     * scheduler handoff. It resumes here only after the waiter is woken.
     */
    __asm__ __volatile__("svc 0" ::: "memory");
    api_lock_until_acquired(slot);
    resume_parked_api(slot);
#if ZZAPP_PERF_COUNTERS
    if (host->perf_svc_sampled)
        zzapp_kernel_perf_service_begin(&g_kernel, ZZAPP_PERF_SERVICE_SVC,
                                        scheduler_perf_stamp());
#endif
#else
    if (!host->api_parked) {
        api_lock_until_acquired(slot);
        resume_parked_api(slot);
    }
#endif
}
void zzapp_host_sleep(uint32_t us)
{
    uint32_t slot = current_slot();

    if (zzapp_kernel_current_thread() < 0)
        return;
    host_slot(slot)->api_parked = 1u;
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    if (g_kernel.api_lock_depth == 1U && host_slot(slot)->perf_svc_sampled)
        zzapp_kernel_perf_service_end(&g_kernel, ZZAPP_PERF_SERVICE_SVC,
                                      scheduler_perf_stamp());
#endif
    (void)zzapp_kernel_api_sleep(&g_kernel, slot,
        (uint64_t)((us + 999u) / 1000u));
#ifndef ZZAPP_HOST_TEST
    __asm__ __volatile__("svc 0" ::: "memory");
    api_lock_until_acquired(slot);
    resume_parked_api(slot);
#if ZZAPP_PERF_COUNTERS
    if (host_slot(slot)->perf_svc_sampled)
        zzapp_kernel_perf_service_begin(&g_kernel, ZZAPP_PERF_SERVICE_SVC,
                                        scheduler_perf_stamp());
#endif
#endif
}


uint64_t zzapp_host_time_us(void) { return ticks_to_us(now_ticks()); }
#ifndef ZZAPP_HOST_TEST
uint64_t zzapp_kernel_now_ms(void)
{
    return zzapp_host_time_us() / 1000u;
}
#else
uint64_t __attribute__((weak)) zzapp_kernel_now_ms(void)
{
    return zzapp_host_time_us() / 1000u;
}
#endif

/*
 * Every GL call of an ARM app passes through here (GLQuake makes tens of
 * thousands per frame). The SVC watchdog state is private to core 1 and
 * cached; service_tick_irq publishes only the diagnostic in_api value.
 */
void zzapp_api_enter(void)
{
    uint32_t slot = current_slot();
    zzapp_rec_host_t *host = host_slot(slot);

    if (zzapp_kernel_current_thread() >= 0) {
        int contended;

#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
        zzapp_svc_gl_telemetry_lock_begin(slot);
#endif
        contended = api_lock_until_acquired(slot);
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
        zzapp_svc_gl_telemetry_lock_end(slot, contended);
        if (g_kernel.api_lock_depth == 1U) {
            host->perf_svc_sampled = zzapp_svc_counters_sampled(slot);
            if (host->perf_svc_sampled)
                zzapp_kernel_perf_service_begin(&g_kernel,
                                                ZZAPP_PERF_SERVICE_SVC,
                                                scheduler_perf_stamp());
        }
#else
        (void)contended;
#endif
    }
    /* Stamp before publishing this call so a timer IRQ never backdates it
     * from this slot's previous scheduling tick. */
    if (host->in_api == 0u)
        host->api_entered = now_ticks();
    host->api_calls++;
    host->in_api++;
    host->api_parked = 0u;
}

/* Hot non-blocking calls (should_stop, time_us). Count as an API call so a
 * loop that only polls them is not a 30 s stall, without the in-API depth
 * or the leave-path service tick. */
void zzapp_api_touch(void)
{
    host_slot(current_slot())->api_calls++;
}

void zzapp_api_leave(void)
{
    uint32_t slot = current_slot();
    zzapp_rec_host_t *host = host_slot(slot);

    if (host->in_api)
        host->in_api--;
    if (zzapp_kernel_current_thread() >= 0) {
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
        if (g_kernel.api_lock_depth == 1U && host->perf_svc_sampled)
            zzapp_kernel_perf_service_end(&g_kernel, ZZAPP_PERF_SERVICE_SVC,
                                          scheduler_perf_stamp());
#endif
        (void)zzapp_kernel_api_unlock(&g_kernel, slot);
#if ZZAPP_PERF_COUNTERS
        if (g_kernel.api_lock_depth == 0U)
            host->perf_svc_sampled = 0U;
#endif
    }
    if (!host->in_api && host->service_due) {
        host->service_due = 0u;
        zzapp_kernel_service_due(&g_kernel);
    }
}

#ifndef ZZAPP_HOST_TEST
static uint8_t *proc_alias(uint32_t offset, uint32_t bytes)
{
    zzapp_proc_t *proc = proc_of(current_slot());
    uint32_t frame = offset >> 20;
    uint32_t in_frame = offset & (ZZAPP_MEM_FRAME_BYTES - 1u);
    uint32_t physical;

    if (bytes > ZZAPP_MEM_FRAME_BYTES - in_frame)
        return NULL;
    if (frame < proc->image_frame_count)
        physical = proc->image_frames[frame];
    else if (offset >= proc->heap_base &&
             frame - (proc->heap_base >> 20) < proc->heap_frame_count)
        physical = proc->heap_frames[frame - (proc->heap_base >> 20)];
    else if (offset >= proc->stack_base &&
             frame - (proc->stack_base >> 20) < proc->stack_frame_count)
        physical = proc->stack_frames[frame - (proc->stack_base >> 20)];
    else
        return NULL;
    return (uint8_t *)(uintptr_t)(physical + in_frame);
}

static int alias_readonly(zzapp_as_t *as, const void *ptr, uint32_t bytes,
                          uint32_t *alias)
{
    uint32_t physical = (uint32_t)(uintptr_t)ptr;
    uint32_t offset = physical & 0xfffU;
    uint32_t span;
    uint32_t mapped;

    if (!ptr || !bytes || !alias || bytes > UINT32_MAX - offset - 0xfffU)
        return -1;
    span = (bytes + offset + 0xfffU) & ~0xfffU;
    if (zzapp_as_alias_map_flags(as, physical - offset, span,
                                 ZZAPP_AS_ALIAS_RO, &mapped))
        return -1;
    *alias = mapped + offset;
    return 0;
}

#endif

static int commit_argv_frames(uint32_t end_offset)
{
    zzapp_proc_t *proc = proc_of(current_slot());
    uint32_t need;
    uint32_t have;

    if (end_offset <= proc->heap_base)
        return AAR_OK;
    need = (end_offset - proc->heap_base + ZZAPP_MEM_FRAME_BYTES - 1u) >> 20;
    have = proc->heap_frame_count;
    if (need <= have)
        return AAR_OK;
    if (need > ZZAPP_MEM_MAX_FRAMES)
        return AAR_E_NOMEM;
#ifndef ZZAPP_HOST_TEST
    if (zzapp_loader_heap_grow((need - have) * ZZAPP_MEM_FRAME_BYTES) != AAR_OK)
        return AAR_E_NOMEM;
#else
    proc->heap_frame_count = need;
    if (proc->heap_break < proc->heap_base + need * ZZAPP_MEM_FRAME_BYTES)
        proc->heap_break = proc->heap_base + need * ZZAPP_MEM_FRAME_BYTES;
#endif
    return AAR_OK;
}

/* One committed frame only. A span that crosses a frame is copied in pieces. */
static uint8_t *argv_frame_alias(uint32_t offset, uint32_t bytes)
{
    zzapp_proc_t *proc = proc_of(current_slot());
    uint32_t in_frame = offset & (ZZAPP_MEM_FRAME_BYTES - 1u);

    if (!bytes || bytes > ZZAPP_MEM_FRAME_BYTES - in_frame)
        return NULL;
#ifdef ZZAPP_HOST_TEST
    {
        zzapp_rec_host_t *host = host_slot(current_slot());
        uint32_t frame = offset >> 20;

        if (!host->mem)
            return NULL;
        if (offset < proc->heap_base) {
            if (proc->image_frame_count != 0u &&
                frame >= proc->image_frame_count)
                return NULL;
            return host->mem + offset;
        }
        if (frame < (proc->heap_base >> 20) ||
            frame - (proc->heap_base >> 20) >= proc->heap_frame_count)
            return NULL;
        return host->mem + offset;
    }
#else
    (void)proc;
    return proc_alias(offset, bytes);
#endif
}

static int write_argv_bytes(uint32_t offset, const void *src, uint32_t bytes)
{
    const uint8_t *from = (const uint8_t *)src;

    while (bytes != 0u) {
        uint32_t in_frame = offset & (ZZAPP_MEM_FRAME_BYTES - 1u);
        uint32_t n = ZZAPP_MEM_FRAME_BYTES - in_frame;
        uint8_t *dst;

        if (n > bytes)
            n = bytes;
        dst = argv_frame_alias(offset, n);
        if (!dst)
            return AAR_E_NOMEM;
        if (from)
            memcpy(dst, from, n);
        else
            memset(dst, 0, n);
        offset += n;
        if (from)
            from += n;
        bytes -= n;
    }
    return AAR_OK;
}

/* The lease the client holds on a running app: the short one, or the
 * extended one a client announced in its LOAD_BEGIN capabilities. */
static uint32_t lease_period_us(const zzapp_rec_t *rec)
{
    return (rec->claim.client_caps & ZZAPP_CLIENT_LEASE_EXTENDED) ?
           ZZAPP_LAUNCHER_LEASE_US : ZZAPP_LEASE_US;
}

/* Copies argc strings from the argv area of the load buffer (offsets from the
 * start of the buffer, ZZAPP_LOAD_ARGV_OFF) into the app's heap and builds its
 * argv table there. */
static int copy_argv(uint32_t argc, char ***argv_out)
{
    zzapp_rec_host_t *host = host_slot(current_slot());
    zzapp_xport_buf_t load;
    const uint8_t *area = NULL;
    uint32_t i;
#ifdef ZZAPP_HOST_TEST
    uint32_t ptr_bytes = (uint32_t)sizeof(char *);
#else
    uint32_t ptr_bytes = sizeof(uint32_t);
#endif
    uint32_t total = (argc + 1u) * ptr_bytes;
    uint32_t argv_offset;
    uint32_t string_at[ZZAPP_ARGV_MAX];
    uint32_t string_n[ZZAPP_ARGV_MAX];

    if (argc > ZZAPP_ARGV_MAX || !host->mem)
        return AAR_E_INVAL;
    if (argc) {
        if (!zzapp_xport_block_get(life_rec(), ZZAPP_XPORT_BLOCK_LOAD,
                                   &load) ||
            load.bytes < ZZAPP_LOAD_ARGV_OFF + ZZAPP_LOAD_ARGV_BYTES)
            return AAR_E_IO;
        area = (const uint8_t *)load.ptr;
    }
    for (i = 0; i < argc; i++) {
        uint32_t off = zzapp_get_be32(area + ZZAPP_LOAD_ARGV_OFF +
                                      ZZAPP_ARGV_OFFS_OFF + 4u * i);
        uint32_t n = 0;

        if (off < ZZAPP_LOAD_ARGV_OFF ||
            off >= ZZAPP_LOAD_ARGV_OFF + ZZAPP_LOAD_ARGV_BYTES)
            return AAR_E_INVAL;
        while (off + n < ZZAPP_LOAD_ARGV_OFF + ZZAPP_LOAD_ARGV_BYTES &&
               area[off + n])
            n++;
        if (off + n >= ZZAPP_LOAD_ARGV_OFF + ZZAPP_LOAD_ARGV_BYTES ||
            n >= ZZAPP_ARGV_STR_MAX)
            return AAR_E_INVAL;
        string_at[i] = off;
        string_n[i] = n;
        total += n + 1u;
    }
    total = (total + 7u) & ~7u;
    if (host->heap_bump + total > host->heap_end)
        return AAR_E_NOMEM;
    if (commit_argv_frames(host->heap_bump + total) != AAR_OK)
        return AAR_E_NOMEM;
    argv_offset = host->heap_bump;
    host->heap_bump += (argc + 1u) * ptr_bytes;
    for (i = 0; i < argc; i++) {
        uint32_t n = string_n[i];
        uint32_t value_off = host->heap_bump;
#ifdef ZZAPP_HOST_TEST
        char *value = (char *)(host->mem + value_off);
#else
        uint32_t value = ZZAPP_AS_WINDOW_BASE + value_off;
#endif

        if (write_argv_bytes(value_off, area + string_at[i], n) != AAR_OK ||
            write_argv_bytes(value_off + n, NULL, 1u) != AAR_OK ||
            write_argv_bytes(argv_offset + i * ptr_bytes, &value,
                             ptr_bytes) != AAR_OK)
            return AAR_E_NOMEM;
        host->heap_bump += n + 1u;
    }
    if (write_argv_bytes(argv_offset + argc * ptr_bytes, NULL, ptr_bytes) != AAR_OK)
        return AAR_E_NOMEM;
#ifdef ZZAPP_HOST_TEST
    *argv_out = (char **)(host->mem + argv_offset);
#else
    *argv_out = (char **)(uintptr_t)(ZZAPP_AS_WINDOW_BASE + argv_offset);
#endif
    return AAR_OK;
}

#ifndef ZZAPP_HOST_TEST
static int protect_firmware_text(void)
{
    if (g_firmware_text_readonly)
        return 1;
    /*
     * Core 0 and core 1 walk the shared MMUTable. The linker range is
     * page-aligned and excludes writable firmware data and literal pools.
     */
    if ((uintptr_t)__firmware_text_start < 0x08200000U ||
        (uintptr_t)__firmware_text_end <= (uintptr_t)__firmware_text_start ||
        !mmu_page_set_readonly_executable(
            (uintptr_t)__firmware_text_start,
            (uintptr_t)__firmware_text_end)) {
        printf("[zzapp] invalid firmware text range %08lx-%08lx\n",
               (unsigned long)__firmware_text_start,
               (unsigned long)__firmware_text_end);
        return 0;
    }
    g_firmware_text_readonly = 1;
    return 1;
}
#endif

/* START could not complete: the claim ends and the record is retired. */
static void start_failed(uint32_t slot, int status)
{
    uint32_t selected = select_teardown_slot(slot);

    zzapp_services_cancel_start(status);
    (void)zzapp_rec_retire(slot);
    test_admission_point();
    restore_teardown_slot(selected);
}

/* A stop request found the record still loading: nothing runs, so the claim
 * ends here. The client's stop is an administrative one and ends it FORCED,
 * unless the claim already carries a reason (a lease that ran out). */
static void abort_loading_stop(void)
{
    uint32_t slot = current_slot();

    if (rec_must(slot)->claim.terminal_reason == ZZAPP_END_NONE)
        zzapp_services_set_terminal(ZZAPP_END_FORCED);
    zzapp_services_cancel_start(AAR_E_IO);
    (void)zzapp_rec_retire(slot);
}

static void run_app(uint32_t argc)
{
    uint32_t slot = current_slot();
    zzapp_rec_host_t *host = host_slot(slot);
    zzapp_rec_coherent_t *coh = coherent(slot);
    char **argv = NULL;
    const aar_api *app_api;
    int rc;
    zzapp_proc_t *proc = proc_of(slot);

#ifdef ZZAPP_HOST_TEST
    if (g_test_fail_start) {
        g_test_fail_start = 0;
        /* The injected failure stands for any preparation failure; all of
         * them follow the START handoff that core 0 may race. */
        test_start_race_point();
        start_failed(slot, AAR_E_IO);
        return;
    }
#endif
    if (!host->image_loaded || !host->mem) { start_failed(slot, AAR_E_INVAL); return; }
    host->heap_off = (host->image_end + 7u) & ~7u;
#ifdef ZZAPP_HOST_TEST
    host->heap_end = ZZAPP_HOST_WINDOW_BYTES - host->stack_bytes;
#else
    host->heap_end = proc->heap_limit;
#endif
    host->heap_bump = host->heap_off;
    if (host->heap_end <= host->heap_off) { start_failed(slot, AAR_E_NOMEM); return; }
    rc = copy_argv(argc, &argv);
    /* The argv area was the last use of the load buffer. */
    (void)zzapp_xport_block_free(life_rec(), ZZAPP_XPORT_BLOCK_LOAD);
    if (rc != AAR_OK) { start_failed(slot, rc); return; }
    host->heap_bump = (host->heap_bump + 7u) & ~7u;
    proc->heap_start = host->heap_bump;
    proc->heap_break = proc->heap_base +
                       proc->heap_frame_count * ZZAPP_MEM_FRAME_BYTES;
    proc->heap_limit = host->heap_end;
#ifndef ZZAPP_HOST_TEST
    if (!protect_firmware_text()) {
        start_failed(slot, AAR_E_IO);
        return;
    }
    {
        uint32_t veneer_alias;
        uint32_t api_alias;
        uint32_t gl_alias;
        aar_gl *gl;

        app_api = zzapp_svc_api_for_slot(slot, zzapp_api_table());
        gl = zzapp_svc_gl_for_slot(slot);
        if (!app_api || !gl ||
            zzapp_as_alias_map_flags(&proc->as,
                (uint32_t)zzapp_svc_api_veneer(0) & ~0xfffU, 4096U,
                ZZAPP_AS_ALIAS_RX, &veneer_alias) ||
            alias_readonly(&proc->as, app_api, sizeof(*app_api), &api_alias) ||
            alias_readonly(&proc->as, gl, sizeof(*gl), &gl_alias) ||
            zzapp_svc_set_veneer_alias(slot, veneer_alias) ||
            zzapp_svc_set_table_aliases(slot, api_alias, gl_alias) ||
            !zzapp_svc_api_for_slot(slot, zzapp_api_table())) {
            start_failed(slot, AAR_E_IO);
            return;
        }
        app_api = (const aar_api *)(uintptr_t)api_alias;
    }
#endif
#ifdef ZZAPP_HOST_TEST
    app_api = zzapp_api_table();
#endif
#ifdef ZZAPP_HOST_TEST
    host->stack_canary = host->heap_end;
    memset(host->mem + host->stack_canary, 0xA5, ZZAPP_CANARY_BYTES);
#else
    {
        uint8_t *canary;

        host->stack_canary = proc->stack_base;
        canary = proc_alias(host->stack_canary, ZZAPP_CANARY_BYTES);
        if (!canary) { start_failed(slot, AAR_E_NOMEM); return; }
        memset(canary, 0xA5, ZZAPP_CANARY_BYTES);
    }
#endif
    host->stop_mark = 0;
    host->fault_mark = 0;
    host->faulted = 0;
    __atomic_store_n(&host->exit_state, ZZAPP_EXIT_NONE, __ATOMIC_RELEASE);
    host->stop_diagnostics_valid = 0u;
    host->exit_code = 0;
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    zzapp_svc_counters_reset(slot);
#endif
    coh->heartbeat_value = cb_client(AAR_CB_LEASE_COUNTER);
    cb_put(AAR_CB_LEASE_PERIOD_MS, lease_period_us(life_rec()) / 1000u);
    coh->heartbeat_seen = now_ticks();
    coh->tick_seen = now_ticks();
    zzapp_note_progress();
    begin_tick_observation();
    coherent_barrier();
    if (zzapp_kernel_prepare_app(&g_kernel, slot,
#ifdef ZZAPP_HOST_TEST
                                 host->card + host->entry,
#else
                                 ZZAPP_AS_WINDOW_BASE + host->entry,
#endif
                                 (uint32_t)(uintptr_t)app_api, argc,
                                 (uint32_t)(uintptr_t)argv,
#ifdef ZZAPP_HOST_TEST
                                 host->card + proc->stack_top
#else
                                 ZZAPP_AS_WINDOW_BASE + proc->stack_top
#endif
                                 ) != 0) {
        start_failed(slot, AAR_E_INVAL);
        return;
    }
    /* Host tests inject core 0 here, between START preparation and its
     * completion: it may expire the claim, so start_entered rejects. */
    test_start_race_point();
    rc = zzapp_services_start_entered(coh->generation);
    if (rc != AAR_OK) {
        /*
         * A cancelled START is completed inside start_entered, which ends
         * the claim's state; the record is retired here.
         */
        test_admission_point();
        (void)zzapp_rec_retire(slot);
        test_admission_point();
        return;
    }
    coh->running = 1;
    publish_state(ZZAPP_ST_RUNNING);
    (void)zzapp_kernel_start_app(&g_kernel, slot);
#ifdef ZZAPP_HOST_TEST
    zzapp_kernel_set_current_thread((int)slot);
    zzapp_private_timer_enable();
    if (g_test_entry)
        g_test_entry(zzapp_api_table(), (int)argc, argv);
    zzapp_private_timer_disable();
    zzapp_kernel_set_current_thread(-1);
    zzapp_worker_hook();
    /* An entry that returns without calling exit ends the same way. */
    if (coh->running && !host->faulted) {
        request_exit(slot);
        complete_pending_exit(slot);
    }
#else
    zzapp_kernel_set_current_thread(-1);
    zzapp_private_timer_enable();
#endif
}

/* Core 1's side of an Amiga reset for one record: whatever it was doing ends
 * and the record is retired. Services releases the held shells afterwards,
 * once (zzapp_on_amiga_reset). */
static void service_reset(uint32_t slot)
{
    zzapp_rec_host_t *host = host_slot(slot);
    zzapp_rec_coherent_t *coh = coherent(slot);
    uint32_t selected;

    if (!coh->reset_request || visible(slot)->commit_active)
        return;
    selected = select_teardown_slot(slot);
    if (host->state == ZZAPP_ST_LOADING || host->state == ZZAPP_ST_STARTING) {
        zzapp_services_cancel_start(AAR_E_IO);
        (void)zzapp_loader_abort();
    } else if (coh->running) {
        force_stop(ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
    }
    if (zzapp_rec_state(slot) != ZZAPP_REC_FREE) {
        zzapp_services_set_terminal(ZZAPP_END_AMIGA_RESET);
        zzapp_services_on_app_end();
        (void)zzapp_rec_retire(slot);
    }
    zzapp_host_clear_stop_requested(slot);
    host->stop_mark = 0;
    host->fault_mark = 0;
    coherent_barrier();
    coh->reset_ack = 1;
    coh->reset_request = 0;
    coherent_publish();
    restore_teardown_slot(selected);
}

static int record_live(uint32_t pid)
{
    return zzapp_rec_state(pid) != ZZAPP_REC_FREE;
}

/* Any record asked to reset: the worker leaves the service poll alone until
 * every one of them has acknowledged. */
static int reset_pending(void)
{
    uint32_t pid;
    uint32_t capacity = zzapp_rec_capacity();

    for (pid = 0U; pid < capacity; ++pid)
        if (record_live(pid) && coherent(pid)->reset_request)
            return 1;
    return 0;
}

void zzapp_worker_hook(void)
{
    uint32_t selected = current_slot();
    uint32_t slot;
    uint32_t capacity = zzapp_rec_capacity();
    zzapp_rec_host_t *host;
    uint32_t argc = 0;
    uint64_t now = now_ticks();

#if ZZAPP_PERF_COUNTERS || defined(ZZAPP_HOST_TEST)
    zzapp_loader_work_begin_pass();
#endif
    for (slot = 0; slot < capacity; ++slot) {
        if (!record_live(slot))
            continue;
        if (coherent(slot)->reset_request && coherent(slot)->running &&
            !visible(slot)->commit_active)
            request_stop(slot, ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
        zzapp_services_select_pid(slot);
        complete_pending_stop(slot);
        complete_pending_exit(slot);
    }
    for (slot = 0; slot < capacity; ++slot) {
        if (!record_live(slot) || !coherent(slot)->reset_request)
            continue;
        zzapp_services_select_pid(slot);
        service_reset(slot);
        /* service_reset returns only after this record's teardown has no
         * worker task in flight, so a pending core-1 cache clean is safe. */
        scheduler_core1_service_clean_request();
    }
    for (slot = 0; slot < capacity; ++slot) {
        if (!record_live(slot))
            continue;
        zzapp_services_select_pid(slot);
        service_watch_slot(now);
        complete_pending_stop(slot);
        complete_pending_exit(slot);
        if (record_live(slot) && host_slot(slot)->state == ZZAPP_ST_LOADING &&
            stop_requested())
            abort_loading_stop();
    }
    zzapp_services_select_pid(selected);
    if (reset_pending())
        return;
    zzapp_services_poll();
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    zzapp_kernel_perf_switch_finished(&g_kernel);
    (void)zzapp_kernel_perf_emit_reports(&g_kernel);
#endif
#ifdef ZZAPP_HOST_TEST
    scheduler_core1_service_from_app();
#endif
    for (slot = 0U; slot < capacity; ++slot) {
        if (!record_live(slot))
            continue;
        zzapp_services_select_pid(slot);
        host = host_slot(slot);
        if (host->state == ZZAPP_ST_LOADING &&
            cb_client(AAR_CB_STOP_REQUEST) != 0u)
            zzapp_host_latch_stop(slot);
        if (host->state == ZZAPP_ST_LOADING && stop_requested()) {
            abort_loading_stop();
            continue;
        }
        argc = 0U;
        if (host->state == ZZAPP_ST_LOADING &&
            zzapp_services_take_start(&argc)) {
            publish_state(ZZAPP_ST_STARTING);
            run_app(argc);
        }
    }
    zzapp_services_select_pid(selected);
}

int zzapp_fault_is_app(uint32_t pc, uint32_t abort_lr, uint32_t spsr)
{
    zzapp_rec_t *rec = selected_rec();
    zzapp_rec_host_t *host;

    /* An API body runs in SYS, and SVC entry is still SVC, while the app
     * stays current. Only a fault that interrupted user mode is the app's. */
    if ((spsr & 0x1fU) != ZZAPP_KERNEL_APP_CPSR)
        return 0;
    if (!rec)
        return 0;
#ifdef ZZAPP_HOST_TEST
    host = &rec->host;
    uint32_t end = host->card + ZZAPP_HOST_WINDOW_BYTES;

    return rec->coh.running && host->card && end > host->card &&
           ((pc >= host->card && pc < end) ||
            (abort_lr >= host->card && abort_lr < end));
#else
    (void)pc;
    (void)abort_lr;
    (void)host;
    return rec->coh.running && zzapp_kernel_current_thread() >= 0;
#endif
}

int zzapp_fault_wants_restart(uint32_t pc, uint32_t abort_lr, uint32_t spsr)
{
    return zzapp_fault_is_app(pc, abort_lr, spsr) ? 0 : 1;
}

void zzapp_take_app_fault(uint32_t fault, uint32_t pc, uint32_t sp,
                          uint32_t lr, uint32_t far, uint32_t fsr,
                          uint32_t cpsr)
{
    (void)zzapp_kernel_api_fault_release(&g_kernel, current_slot());
    request_stop(current_slot(), fault, pc, sp, lr, far, fsr, cpsr);
}

/*
 * Age of a timestamp that core 1 keeps rewriting.  A 64-bit store is not
 * single-copy atomic on the Cortex-A9 (no LPAE), so re-read until two reads
 * agree, and read the clock only afterwards: a stamp written between an
 * earlier clock read and the stamp read lies "in the future", and the
 * unsigned difference became a huge age that cold-restarted a healthy core 1.
 */
static uint64_t core1_stamp_age_us(const volatile uint64_t *stamp)
{
    uint64_t a;
    uint64_t b;
    uint64_t now;

    do {
        a = *stamp;
        b = *stamp;
    } while (a != b);
    now = now_ticks();
    return now > a ? ticks_to_us(now - a) : 0u;
}

void zzapp_core0_poll(void)
{
    uint32_t slot;
    uint32_t capacity = zzapp_rec_capacity();
    uint32_t selected = current_slot();
    int restart = 0;
    uint32_t restart_slot = 0U;
    uint64_t restart_age = 0U;
    uint32_t restart_in_api = 0U;
    zzapp_services_expire_claims(ticks_to_us(now_ticks()));
    uint64_t core_tick_age = core1_stamp_age_us(g_core_heartbeat);

    for (slot = 0U; slot < capacity; ++slot) {
        zzapp_rec_host_t *host;
        zzapp_rec_coherent_t *coh;
        uint32_t running;
        uint64_t age;
        uint64_t now;
        uint32_t heartbeat;

        if (!record_live(slot))
            continue;
        host = host_slot(slot);
        coh = coherent(slot);
        running = coh->running;
        zzapp_services_select_pid(slot);
        coherent_barrier();
        age = core1_stamp_age_us(&coh->progress);
        now = now_ticks();
        heartbeat = cb_client(AAR_CB_LEASE_COUNTER);
        maybe_expire_fault(now);
        if (heartbeat != coh->heartbeat_value) {
            coh->heartbeat_value = heartbeat;
            coh->heartbeat_seen = now;
        }
        if (running && host->state == ZZAPP_ST_RUNNING &&
            core1_stamp_age_us(&coh->heartbeat_seen) >=
                lease_period_us(zzapp_rec_by_pid(slot))) {
            /* The lease ran out. Only the record's own stop latches; the
             * client's stop word is the client's to write. */
            host->terminal_reason = ZZAPP_END_LEASE;
            zzapp_host_latch_stop(slot);
        }
        /* One stale record restarts the shared worker once. Clearing running
         * here would hide the other records from the retirement below. */
        if (running && age >= ZZAPP_WATCHDOG_US &&
            core_tick_age >= ZZAPP_WATCHDOG_US && !restart) {
            restart = 1;
            restart_slot = slot;
            restart_age = age;
            restart_in_api = coh->in_api;
        }
    }
    if (restart) {
#ifndef ZZAPP_HOST_TEST
        printf("[app] watchdog restart: pid %lu no progress for %lu ms, "
               "no core tick for %lu ms, in_api=%lu\r\n",
               (unsigned long)restart_slot,
               (unsigned long)(restart_age / 1000u),
               (unsigned long)(core_tick_age / 1000u),
               (unsigned long)restart_in_api);
        core1_cold_restart();
#else
        zzapp_recover_halted_core1();
#endif
    }
#ifdef ZZAPP_HOST_TEST
    (void)restart_slot;
    (void)restart_age;
    (void)restart_in_api;
#endif
    zzapp_services_select_pid(selected);

}

#ifdef ZZAPP_HOST_TEST
extern void zzapp_mem_reset_live_lock(void) __attribute__((weak));
extern void zzapp_mem_reconcile_halted(void) __attribute__((weak));
/* The card's version (scheduler_arm.c) resets every lock core 1 could hold.
 * The harness has the address-space, record and ledger layers to reset. */
void __attribute__((weak)) zzapp_recover_external_locks(void)
{
    zzapp_as_reset_lock();
    (void)zzapp_rec_reconcile_halted();
    zzapp_as_reset_asids();
    if (zzapp_mem_reset_live_lock)
        zzapp_mem_reset_live_lock();
    if (zzapp_mem_reconcile_halted)
        zzapp_mem_reconcile_halted();
}
#endif
void zzapp_recover_halted_core1(void)
{
    zzapp_kernel_t *kernel = zzapp_runtime_kernel();
    uint32_t pid;
    uint32_t capacity = zzapp_rec_capacity();

    /* Every lock core 1 could hold, before any teardown acquires one.
     * Resetting the kernel lock wakes its waiters, so contexts are
     * stopped after that and before mappings are released. */
    zzapp_recover_external_locks();
    zzapp_kernel_reset_lock(kernel);
    if (kernel) {
        for (pid = 0U; pid < capacity; pid++) {
            zzapp_thread_t *thread = zzapp_kernel_thread(kernel, pid);

            zzapp_kernel_stop_app(kernel, pid);
            if (thread)
                memset(&thread->context, 0, sizeof(thread->context));
        }
        zzapp_kernel_reset_slice(kernel);
    }
    zzapp_kernel_set_current_thread(-1);
    zzapp_on_core1_restart();
}

/* One record ends without an app to stop: its claim, a load in progress, or
 * a start that never completed. Stage 3's services hook, then the retire. */
static void end_record(uint32_t pid, uint32_t reason)
{
    uint32_t selected = select_teardown_slot(pid);

    zzapp_services_set_terminal(reason);
    zzapp_services_on_app_end();
    host_slot(pid)->state = ZZAPP_ST_IDLE;
    coherent_publish();
    (void)zzapp_rec_retire(pid);
    restore_teardown_slot(selected);
}

/* Core 1 restarted (watchdog, or an unacknowledged reset): every record is
 * torn down on core 0 while core 1 is halted, through the retire path the
 * worker would have run. Replays of a retire the halt interrupted are safe,
 * each stage is idempotent. */
void zzapp_on_core1_restart(void)
{
    uint32_t slot;
    uint32_t capacity = zzapp_rec_capacity();
    uint32_t selected = current_slot();

    g_finish_tlb_on_stop = 0;
    for (slot = 0U; slot < capacity; ++slot) {
        zzapp_rec_coherent_t *coh;
        /* A reset core 1 never acknowledged ends the record too. */
        int amiga_reset;
        uint32_t reason;

        if (!record_live(slot))
            continue;
        coh = coherent(slot);
        amiga_reset = coh->reset_request != 0u;
        reason = amiga_reset ? ZZAPP_END_AMIGA_RESET : ZZAPP_END_CORE_RESTART;
        /* A commit the halted core was in the middle of can never finish, so
         * its guard is dropped first. */
        visible(slot)->commit_active = 0u;
        zzapp_services_select_pid(slot);
        if (slot_owes_teardown(slot)) {
            /* An app whose exit the dead core 1 never finished tearing
             * down is torn down like a running one: its host state is
             * still RUNNING, which an abort would refuse. */
            host_slot(slot)->terminal_reason = reason;
            force_stop(ZZAPP_FAULT_WATCHDOG_RESTART, 0, 0, 0, 0, 0, 0);
            __atomic_store_n(&host_slot(slot)->exit_state, ZZAPP_EXIT_NONE,
                             __ATOMIC_RELEASE);
        } else if (zzapp_rec_state(slot) != ZZAPP_REC_RECLAIMED) {
            end_record(slot, reason);
        }
        /* A shell whose retire finished (RECLAIMED) owes nothing: it keeps
         * the end it already reports, which a restart does not rewrite. */
        if (amiga_reset) {
            /* The reset core 1 never acknowledged is answered here. Its
             * records are retired already; held shells are released once
             * in zzapp_on_amiga_reset. The restarted worker finds no reset
             * request, no deferred work and no running app, so it owns
             * nothing the allocator reset could invalidate. */
            host_slot(slot)->stop_mark = 0;
            host_slot(slot)->fault_mark = 0;
            coherent_barrier();
            coh->reset_ack = 1;
            coh->reset_request = 0;
            coherent_publish();
        }
    }
    g_finish_tlb_on_stop = 1;
    zzapp_services_select_pid(selected);
    g_cold_restarts++;
}

/* One shared deadline for every record to acknowledge a reset: a running app
 * stops itself within one private-timer tick and the worker acks after its
 * teardown, so the time grows with how many records core 1 has to walk, not
 * with their number times a second. Bounded well under the 2 s API-stall
 * limit per record and by 4 s in all. */
#define ZZAPP_RESET_ACK_BASE_US       1000000u
#define ZZAPP_RESET_ACK_PER_RECORD_US 20000u
#define ZZAPP_RESET_ACK_MAX_US        4000000u

uint32_t zzapp_reset_deadline_us(uint32_t live_records)
{
    uint32_t us;

    if (live_records > (ZZAPP_RESET_ACK_MAX_US - ZZAPP_RESET_ACK_BASE_US) /
                       ZZAPP_RESET_ACK_PER_RECORD_US)
        return ZZAPP_RESET_ACK_MAX_US;
    us = ZZAPP_RESET_ACK_BASE_US + live_records * ZZAPP_RESET_ACK_PER_RECORD_US;
    return us;
}

static void request_slot_reset(uint32_t slot)
{
    coherent(slot)->reset_ack = 0;
    coherent(slot)->reset_request = 1;
    host_slot(slot)->terminal_reason = ZZAPP_END_AMIGA_RESET;
    zzapp_host_latch_stop(slot);
}

/* The first record that has not acknowledged its reset, or ZZAPP_PID_NONE.
 * A record that was retired by another path (an exit that raced the request)
 * is gone and needs no acknowledgement. */
static uint32_t reset_first_unacked(void)
{
    uint32_t pid;
    uint32_t capacity = zzapp_rec_capacity();

    for (pid = 0U; pid < capacity; ++pid) {
        if (record_live(pid) && coherent(pid)->reset_request &&
            !coherent(pid)->reset_ack)
            return pid;
    }
    return ZZAPP_PID_NONE;
}

/* Core 1 acknowledges the reset from its own reset path, which is also what
 * retires the record: it tears down a running app, an app that exited and
 * whose cleanup has not finished, and a loading or claimed record. Core 0
 * waits for every record before it lets services release the held shells and
 * resets the surface allocator, so no cleanup frees allocations from a
 * previous life. With no core 1 there is nothing to wait for. */
static int reset_must_wait_for_core1(void)
{
#ifdef ZZAPP_HOST_TEST
    return 1;
#else
    return scheduler_core1_available();
#endif
}

#ifdef ZZAPP_HOST_TEST
/* One step of the core 1 side while core 0 waits. A test overrides it to
 * interleave the two. `pid` is the first record still waiting. */
void __attribute__((weak)) zzapp_test_core1_step(uint32_t pid, unsigned pass)
{
    (void)pid;
    (void)pass;
    zzapp_worker_hook();
}
#endif

/* Core 1 did not acknowledge a reset in time. It may only be slow, and a
 * slow worker goes on to run the reset on its records later, after the
 * allocator was reset and its addresses reused. Core 0 therefore does not
 * clean up beside it. Core 1 is halted first, every record is recovered while
 * it is held, and then it is released with nothing left to do. */
static void recover_unresponsive_core1(void)
{
#ifdef ZZAPP_HOST_TEST
    zzapp_recover_halted_core1();
#else
    core1_cold_restart();
#endif
}

/* After a reset, every record that is still held (a shell whose client has
 * not released it, a quarantined shell) is released: its host-visible blocks
 * and its transport go back to the allocator, which is reset next, and the
 * record is freed. The record is detached from every pointer before its
 * block is returned (zzapp_xport_release clears first), so a replayed reset
 * frees nothing twice. Core 1 is finished with every record. */
static void release_held_shells(void)
{
    uint32_t pid;
    uint32_t capacity = zzapp_rec_capacity();
    uint32_t selected = current_slot();

    for (pid = 0U; pid < capacity; ++pid) {
        zzapp_rec_t *rec;

        if (zzapp_rec_state(pid) != ZZAPP_REC_RECLAIMED)
            continue;
        rec = zzapp_rec_by_pid(pid);
        zzapp_services_select_pid(pid);
        zzapp_xport_flag_clear(rec, ZZAPP_XPORT_F_QUARANTINED);
        host_visible_release_all();
        (void)zzapp_xport_release(rec);
        (void)zzapp_rec_free(rec->hdr.handle);
    }
    zzapp_services_select_pid(selected);
}

void zzapp_on_amiga_reset(void)
{
    uint32_t pid;
    uint32_t capacity = zzapp_rec_capacity();
    uint32_t selected = current_slot();
    uint32_t live = 0U;
    int core1_unresponsive = 0;

    coherent_barrier();
    for (pid = 0U; pid < capacity; ++pid) {
        if (!record_live(pid))
            continue;
        request_slot_reset(pid);
        live++;
    }
    coherent_publish();
    wake_core1();
    if (reset_must_wait_for_core1()) {
        uint64_t start = now_ticks();
        uint32_t deadline = zzapp_reset_deadline_us(live);
#ifdef ZZAPP_HOST_TEST
        unsigned pass = 0U;
#endif

        while (reset_first_unacked() != ZZAPP_PID_NONE &&
               ticks_to_us(now_ticks() - start) < deadline) {
#ifdef ZZAPP_HOST_TEST
            zzapp_test_core1_step(reset_first_unacked(), pass++);
            /* Model the platform sleep when the test supplies no clock.
             * A time hook owns elapsed time, not the interleave hook. */
            if (!g_test_time_hook)
                g_test_ticks += 100U;
#else
            usleep(100u);
#endif
        }
        if (reset_first_unacked() != ZZAPP_PID_NONE) {
            core1_unresponsive = 1;
#ifndef ZZAPP_HOST_TEST
            printf("[app] reset: core 1 did not acknowledge pid %lu within "
                   "%lu ms\r\n",
                   (unsigned long)reset_first_unacked(),
                   (unsigned long)(deadline / 1000u));
#endif
        }
    }
    if (core1_unresponsive) {
        recover_unresponsive_core1();
    } else if (!reset_must_wait_for_core1()) {
        /* No core 1, and the allocator is reset after this: claims end here. */
        for (pid = 0U; pid < capacity; ++pid)
            if (record_live(pid))
                end_record(pid, ZZAPP_END_AMIGA_RESET);
    }
    /* Every record is retired. The held shells release theirs, once: the
     * allocator is reset after this, and a block still owned by a record
     * would be freed against a new owner. */
    zzapp_services_on_reset();
    release_held_shells();
    zzapp_services_select_pid(selected);
}

/* A table the BSP-change hook could not allocate for a record: nothing that
 * could report it to the app is running, so the record stops with a fault
 * (never the kernel). */
static void table_fault_hook(uint32_t pid)
{
    if (!zzapp_rec_by_pid(pid) || !record_live(pid))
        return;
    request_stop(pid, ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
}

#ifdef ZZAPP_HOST_TEST

/* The harness record arena: one mapping below 4 GiB (table addresses go into
 * 32-bit descriptors), laid out again on every boot.
 *
 * The BSP table the kernel template starts from maps all of the 32-bit space
 * 1:1 as write-back shareable DDR (the attributes of the board's DDR
 * sections): tests use card addresses anywhere in it, and an alias map needs
 * a small-page source for every page it grants. */
#define ZZAPP_HOST_BSP_DDR_SECTION 0x15de6U
#define ZZAPP_HOST_BSP_DDR_SECTIONS 0x1000U
static uint32_t g_test_bsp[4096];

/* The harness stand-in for the host-visible heap: aligned host memory under a
 * synthetic card address. A test that needs to refuse or place blocks
 * installs its own environment with zzapp_xport_init() after the boot. */
#define ZZAPP_HOST_XPORT_BASE 0x04000000u
static uint32_t g_test_xport_next;

static int test_xport_alloc(void *ctx, uint32_t bytes, uint32_t align,
                            uint32_t lo, uint32_t hi, void **ptr,
                            uint32_t *card)
{
    void *mem = NULL;
    uint32_t at;

    (void)ctx;
    (void)lo;
    (void)hi;
    if (align < 8u || posix_memalign(&mem, align, bytes) != 0)
        return -1;
    at = (ZZAPP_HOST_XPORT_BASE + g_test_xport_next + align - 1u) &
         ~(align - 1u);
    g_test_xport_next = at - ZZAPP_HOST_XPORT_BASE + bytes;
    *ptr = mem;
    *card = at;
    return 0;
}

static int test_xport_release(void *ctx, void *ptr, uint32_t card)
{
    (void)ctx;
    (void)card;
    free(ptr);
    return 0;
}

static const zzapp_xport_env_t g_test_xport_env = {
    .alloc = test_xport_alloc,
    .release = test_xport_release,
    .token_adjust = ADDR_ADJ,
    .acquire = zzapp_xport_dmb,
    .release_barrier = zzapp_xport_dmb,
};

static void host_arena_init(void)
{
    static void *mapping;
    static size_t mapping_bytes;
    zzapp_rec_layout_t lay;
    uintptr_t base;

    zzapp_rec_layout_for(ZZAPP_HOST_ARENA_APPS, 78u << 20, &lay);
    if (!mapping) {
        mapping_bytes = (size_t)lay.bytes + ZZAPP_REC_L1_BYTES;
        mapping = mmap(NULL, mapping_bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT |
                       MAP_NORESERVE, -1, 0);
        if (mapping == MAP_FAILED)
            abort();
    }
    base = ((uintptr_t)mapping + ZZAPP_REC_L1_BYTES - 1U) &
           ~(uintptr_t)(ZZAPP_REC_L1_BYTES - 1U);
    if (g_test_arena_apps == 0U) {
        /* The boot-time carve found no memory (main.c): no arena. */
        (void)zzapp_rec_arena_init(NULL, &lay);
    } else {
        zzapp_rec_layout_for(g_test_arena_apps, 78u << 20, &lay);
        memset((void *)base, 0, lay.bytes);
        if (zzapp_rec_arena_init((void *)base, &lay) != 0)
            abort();
    }
    /* The kernel template of the address spaces, from the DDR table above. */
    for (uint32_t i = 0; i < ZZAPP_HOST_BSP_DDR_SECTIONS; i++)
        g_test_bsp[i] = (i << 20) | ZZAPP_HOST_BSP_DDR_SECTION;
    zzapp_as_init(g_test_bsp);
    zzapp_xport_init(&g_test_xport_env);
}

void zzapp_test_set_arena_apps(uint32_t apps)
{
    if (apps != 0U && apps < ZZAPP_REC_MIN_APPS)
        apps = ZZAPP_REC_MIN_APPS;
    if (apps > ZZAPP_HOST_ARENA_APPS)
        apps = ZZAPP_HOST_ARENA_APPS;
    g_test_arena_apps = apps;
}

uint32_t zzapp_test_admit(void)
{
    uint32_t handle;
    zzapp_rec_t *rec;

    if (zzapp_rec_admit(&handle, &rec) != 0U)
        return ZZAPP_PID_NONE;
    if (zzapp_xport_admit(rec) != AAR_REASON_NONE) {
        (void)zzapp_rec_unadmit(handle);
        return ZZAPP_PID_NONE;
    }
    zzapp_services_select_pid(rec->hdr.pid);
    zzapp_host_claimed(rec->hdr.pid);
    return rec->hdr.pid;
}

#endif

#ifndef ZZAPP_HOST_TEST
/* The card's host-visible allocator for the transport: the surface heap. A
 * pair and the service buffers are placed below the SDK mailbox section, the
 * cacheable part of the heap, so a block never lands in a page the firmware
 * maps non-cacheable. The card address is the ARM address (flat map). */
static int xport_card_alloc(void *ctx, uint32_t bytes, uint32_t align,
                            uint32_t lo, uint32_t hi, void **ptr,
                            uint32_t *card)
{
    uint32_t addr = (lo | hi) ?
                    surface_allocator_alloc_range(bytes, align, lo, hi) :
                    surface_allocator_alloc_aligned(bytes, align);

    (void)ctx;
    if (!addr)
        return -1;
    *ptr = (void *)(uintptr_t)addr;
    *card = addr;
    return 0;
}

static int xport_card_release(void *ctx, void *ptr, uint32_t card)
{
    (void)ctx;
    (void)ptr;
    return surface_allocator_free(card) == 0 ? 0 : -1;
}

static const zzapp_xport_env_t g_card_xport_env = {
    .alloc = xport_card_alloc,
    .release = xport_card_release,
    .lo = HOST_VISIBLE_HEAP_ADDRESS,
    .hi = SDK_MAILBOX_Z3_ADDRESS,
    .token_adjust = ADDR_ADJ,
    .acquire = zzapp_xport_dmb,
    .release_barrier = zzapp_xport_dmb,
};
#endif
void zzapp_boot_init(void)
{
#ifdef ZZAPP_HOST_TEST
    host_arena_init();
    g_test_xport_next = 0u;
#else
    zzapp_xport_init(&g_card_xport_env);
#endif
    /* Without a record arena (the boot carve found no memory, apps are
     * unavailable) the kernel still needs a valid state: the shared worker
     * takes the system lock at start, and a zeroed static kernel reads as
     * record 0 owning it. */
    if (zzapp_kernel_init(&g_kernel, zzapp_rec_by_pid(0),
                          ZZAPP_REC_RECORD_BYTES, zzapp_rec_capacity()) != 0)
        zzapp_kernel_init_system_only(&g_kernel);
    zzapp_as_set_table_fault_hook(table_fault_hook);
    zzapp_loader_memory_init();
    g_cold_restarts = 0;
    g_kernel.slice_started_ms = zzapp_kernel_now_ms();
    g_kernel.slice_valid = 1U;
#ifdef ZZAPP_HOST_TEST
    g_test_service_hook = NULL;
    g_test_service_calls = 0;
    memset(g_test_wait_registered, 0, sizeof(g_test_wait_registered));
    g_test_time_hook = NULL;
    g_weak_selected = 0U;
#endif
}

void zzapp_host_exit(int code)
{
    uint32_t slot = current_slot();
    zzapp_rec_host_t *host = host_slot(slot);

    host->exit_code = (uint32_t)code;
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    zzapp_svc_counters_report(slot);
    zzapp_kernel_perf_finish_slot(&g_kernel, slot);
    zzapp_kernel_perf_request_report(&g_kernel, slot, 1);
#endif
    if (zzapp_kernel_current_thread() >= 0) {
        request_exit(slot);
#ifndef ZZAPP_HOST_TEST
        /* The system thread runs the teardown. Hand it the CPU; this thread
         * is stopped and is never scheduled again. */
        __asm__ __volatile__("svc #0" ::: "memory");
        for (;;)
            __asm__ __volatile__("wfe" ::: "memory");
#endif
    }
}

void zzapp_host_commit_begin(void)
{
    visible(current_slot())->commit_active = 1u;
}

void zzapp_host_commit_end(void)
{
    visible(current_slot())->commit_active = 0u;
}

/* Granule of host-visible allocations: shared buffers are whole pages. */
#define ZZAPP_SHARED_GRANULE 4096u

/* One host-visible block for the current slot, `bytes` rounded up to the
 * granule and `align` a power of two of at least 8. Used by shared_alloc and
 * by map_file. */
void *zzapp_host_alloc(uint32_t bytes, uint32_t align)
{
    uint32_t slot = current_slot();
    zzapp_rec_visible_t *vis = visible(slot);
    zzapp_host_visible_block_t *blocks = vis->block;
    uint32_t i;

    if (!bytes || align < 8u || (align & (align - 1u)) != 0u ||
        bytes > UINT32_MAX - (ZZAPP_SHARED_GRANULE - 1u))
        return NULL;
    bytes = (bytes + ZZAPP_SHARED_GRANULE - 1u) &
            ~(ZZAPP_SHARED_GRANULE - 1u);
    if (bytes > host_visible_limit(slot) ||
        vis->used + zzapp_xport_shared_used(rec_must(slot)) >
            host_visible_limit(slot) - bytes)
        return NULL;
    if (align < ZZAPP_SHARED_GRANULE)
        align = ZZAPP_SHARED_GRANULE;
    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i) {
        uint32_t card;
        uint32_t alias;
        void *ptr;

        if (blocks[i].ptr)
            continue;
        zzapp_host_commit_begin();
#ifndef ZZAPP_HOST_TEST
        card = surface_allocator_alloc_aligned(bytes, align);
        if (!card || host_visible_alias_map(slot, card, bytes, align,
                                            &alias) != 0) {
            if (card)
                (void)surface_allocator_free(card);
            vis->commit_active = 0u;
            return NULL;
        }
        ptr = (void *)(uintptr_t)alias;
#else
        ptr = malloc(bytes);
        card = 0x033f0000u + vis->used;
        alias = 0u;
        if (ptr && zzapp_test_card_alloc) {
            /* alias carries the allocation serial in host tests */
            card = zzapp_test_card_alloc(bytes, align, &alias);
            if (!card) {
                free(ptr);
                ptr = NULL;
            }
        }
        extern void zzapp_test_host_visible_allocated(void) __attribute__((weak));
        if (zzapp_test_host_visible_allocated)
            zzapp_test_host_visible_allocated();
#endif
        if (!ptr) {
            vis->commit_active = 0u;
            return NULL;
        }
        blocks[i].ptr = ptr;
        blocks[i].card = card;
        blocks[i].alias = alias;
        blocks[i].bytes = bytes;
        vis->used += bytes;
        zzapp_xport_publish_shared(rec_must(slot));
        vis->commit_active = 0u;
        return ptr;
    }
    return NULL;
}

/* Frees a live block of the current slot; AAR_E_INVAL when `p` is not the
 * base of one. */
int zzapp_host_free(void *p)
{
    uint32_t slot = current_slot();
    zzapp_rec_visible_t *vis = visible(slot);
    zzapp_host_visible_block_t *blocks = vis->block;
    uint32_t i;

    if (!p)
        return AAR_E_INVAL;
    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i) {
        if (blocks[i].ptr != p)
            continue;
        vis->commit_active = 1u;
        {
            zzapp_host_visible_block_t rec = blocks[i];

            /* Detached first, see host_visible_release_all. */
            blocks[i].ptr = NULL;
            vis->used -= rec.bytes;
            zzapp_xport_publish_shared(rec_must(slot));
            host_visible_retire(slot, &rec);
        }
        vis->commit_active = 0u;
        return AAR_OK;
    }
    return AAR_E_INVAL;
}

void zzapp_host_heap(void **base, uint32_t *bytes)
{
    uint32_t slot = current_slot();
    zzapp_rec_host_t *host = host_slot(slot);
    zzapp_proc_t *proc = proc_of(slot);

    if (base)
#ifdef ZZAPP_HOST_TEST
        *base = host->mem ? host->mem + proc->heap_start : NULL;
#else
        *base = host->mem ? (void *)(uintptr_t)(ZZAPP_AS_WINDOW_BASE +
                                                proc->heap_start) : NULL;
#endif
    if (bytes)
        *bytes = proc->heap_break > proc->heap_start ?
                 proc->heap_break - proc->heap_start : 0;
}

/* aar_shared_alloc: flags must be 0, `bytes` and `align` round up to the
 * granule. The token is the board-relative Zorro offset of the block. */
int zzapp_host_shared_alloc(uint64_t bytes, uint32_t align, uint32_t flags,
                            struct aar_shared *out)
{
    zzapp_host_visible_block_t *blocks = visible(current_slot())->block;
    uint32_t i;
    void *ptr;

    if (!out || flags != 0u || bytes == 0u)
        return AAR_E_INVAL;
    if (align == 0u)
        align = ZZAPP_SHARED_GRANULE;
    if ((align & (align - 1u)) != 0u)
        return AAR_E_INVAL;
    if (bytes > UINT32_MAX - (ZZAPP_SHARED_GRANULE - 1u))
        return AAR_E_NOMEM;
    ptr = zzapp_host_alloc((uint32_t)bytes, align < 8u ? 8u : align);
    if (!ptr)
        return AAR_E_NOMEM;
    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i)
        if (blocks[i].ptr == ptr)
            break;
    if (i == ZZAPP_HOST_VISIBLE_SLOTS) {
        (void)zzapp_host_free(ptr);
        return AAR_E_IO;
    }
    memset(out, 0, sizeof(*out));
    out->bytes = blocks[i].bytes;
    out->haddr = blocks[i].card - ADDR_ADJ;
    out->flags = AAR_SHAREDF_COHERENT;
    out->ptr = ptr;
    return AAR_OK;
}

/* publish and acquire: the range must lie inside one live shared block. The
 * ZZ9000's shared memory is coherent, so the caller only adds a barrier. */
int zzapp_host_shared_range(const void *p, uint64_t bytes)
{
    zzapp_host_visible_block_t *blocks = visible(current_slot())->block;
    uintptr_t start = (uintptr_t)p;
    uint32_t i;

    if (!p || bytes == 0u)
        return AAR_E_INVAL;
    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i) {
        uintptr_t base = (uintptr_t)blocks[i].ptr;

        if (!blocks[i].ptr || start < base)
            continue;
        if (bytes <= blocks[i].bytes && start - base <= blocks[i].bytes - bytes)
            return AAR_OK;
    }
    return AAR_E_INVAL;
}

int zzapp_host_info(struct aar_info *out)
{
    uint32_t slot = current_slot();
    struct aar_info info;
    uint32_t size;

    if (!out)
        return AAR_E_INVAL;
    size = out->size;
    memset(&info, 0, sizeof(info));
    info.size = size;
    info.profile = AAR_PROFILE_A32;
    info.platform_id = AAR_PLATFORM_ZZ9000;
    info.flags = AAR_INFOF_SHARED_COHERENT | AAR_INFOF_ISOLATED;
    info.groups = zzapp_runtime_groups();
    info.heap_granule = ZZAPP_MEM_FRAME_BYTES;
    info.shared_granule = ZZAPP_SHARED_GRANULE;
    info.msg_capacity = AAR_MSG_CAPACITY;
    info.sleep_granule_us = ZZAPP_TICK_US;
    info.stall_limit_ms = ZZAPP_APP_STALL_US / 1000u;
    info.stop_grace_ms = ZZAPP_STOP_GRACE_US / 1000u;
    info.cpu_features = 0u;
    info.shared_limit = host_visible_limit(slot);
    info.shared_used = visible(slot)->used +
                       zzapp_xport_shared_used(rec_must(slot));
    info.max_files = ZZAPP_FD_COUNT;
    memcpy(out, &info, size < sizeof(info) ? size : sizeof(info));
    return AAR_OK;
}
#ifdef ZZAPP_HOST_TEST
void zzapp_test_set_host_visible_heap(uint32_t bytes)
{
    g_test_host_visible_heap = bytes;
}
/* Replaces the group mask zzapp_runtime_groups() reports; `override` 0
 * restores the runtime's own. */
void zzapp_test_set_groups(uint32_t groups, int override)
{
    g_test_groups = groups;
    g_test_groups_override = override;
}
void zzapp_test_set_xtime(uint64_t ticks) { g_test_ticks = ticks; }
void zzapp_test_set_entry(void (*fn)(const void *api, int argc, char **argv)) { g_test_entry = fn; }
void zzapp_host_tick_at(uint64_t now) { g_test_ticks = now; service_tick_irq(now); }
void zzapp_test_system_tick_at(uint64_t now)
{
    g_test_ticks = now;
    g_test_system_tick = 1U;
    service_tick_irq(now);
    g_test_system_tick = 0U;
}
void zzapp_test_set_time_hook(uint64_t (*hook)(void)) { g_test_time_hook = hook; }
void zzapp_test_request_reset(uint32_t pid)
{
    if (!zzapp_rec_by_pid(pid))
        return;
    request_slot_reset(pid);
    coherent_publish();
}
void zzapp_test_mark_running(void)
{
    uint32_t slot = current_slot();

    coherent(slot)->running = 1;
    host_slot(slot)->state = ZZAPP_ST_RUNNING;
    zzapp_note_progress();
    begin_tick_observation();
    g_kernel.slice_started_ms = zzapp_kernel_now_ms();
    g_kernel.slice_valid = 1U;
}
uint32_t zzapp_test_service_calls(void) { return g_test_service_calls; }
uint32_t zzapp_test_in_api(void) { return host_slot(current_slot())->in_api; }
int zzapp_test_cold_restarts(void) { return g_cold_restarts; }
uint32_t zzapp_test_exit_code(void) { return host_slot(current_slot())->exit_code; }
uint32_t zzapp_test_exit_state(uint32_t pid)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

    return rec ? rec->host.exit_state : 0u;
}
uint8_t *zzapp_test_stack_canary(void)
{
    zzapp_rec_host_t *host = host_slot(current_slot());

    return (host->mem && host->stack_canary != 0u) ?
           host->mem + host->stack_canary : 0;
}
uint32_t zzapp_test_fault_code(void) { return coherent(current_slot())->fault; }
void zzapp_test_set_wait_registered(uint32_t pid, int registered)
{
    if (pid < ZZAPP_REC_MAX_APPS)
        g_test_wait_registered[pid] = registered ? 1u : 0u;
}
#endif
