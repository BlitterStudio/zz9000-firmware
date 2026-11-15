/*
 * Trusted app lifetime, watchdog, service tick, and fault state.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "zzapp.h"
#include "zzapp_proto.h"
#include "zzapp_runtime.h"
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
#endif

extern void zzapp_mem_set_low_memory_callback(
    zzapp_mem_t *memory, zzapp_mem_low_callback_t callback, void *context)
    __attribute__((weak));

#ifdef ZZAPP_HOST_TEST
static uint8_t g_test_wait_registered[ZZAPP_AS_SLOT_COUNT];
void __attribute__((weak)) scheduler_core1_service_clean_request(void)
{
}

#endif

uint32_t __attribute__((weak)) zzapp_services_current_slot(void)
{
    int slot = zzapp_kernel_current_thread();

    return slot >= 0 && slot < (int)ZZAPP_AS_SLOT_COUNT ? (uint32_t)slot : 0U;
}

void __attribute__((weak)) zzapp_services_select_slot(uint32_t slot)
{
    (void)slot;
}

uint32_t __attribute__((weak)) zzapp_services_memory_limit(void)
{
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
    (void)reason;
}

void __attribute__((weak)) zzapp_services_expire_claims(uint64_t now)
{
    (void)now;
}

void __attribute__((weak)) zzapp_services_set_low_memory_crossings(
    uint32_t slot, uint32_t crossings)
{
    (void)slot;
    (void)crossings;
}

int __attribute__((weak)) zzapp_services_wait_registered(uint32_t slot)
{
#ifdef ZZAPP_HOST_TEST
    return slot < ZZAPP_AS_SLOT_COUNT && g_test_wait_registered[slot];
#else
    (void)slot;
    return 0;
#endif
}

struct zzapp_coherent {
    volatile uint32_t running;
    volatile uint32_t in_api;
    volatile uint32_t generation;
    volatile uint32_t fault;
    volatile uint64_t progress;
    volatile uint64_t tick_seen;
    volatile uint64_t core_heartbeat;
    volatile uint64_t heartbeat_seen;
    volatile uint32_t heartbeat_value;
    volatile uint32_t arena_card;
    volatile uint32_t arena_bytes;
    volatile uint32_t owner;
    volatile uint32_t reset_request;
    volatile uint32_t reset_ack;
};

#ifndef ZZAPP_HOST_TEST
static struct zzapp_coherent *const g_coh =
    (struct zzapp_coherent *)(SDK_TASKQ_REGION_ADDRESS + ZZAPP_COHERENT_OFFSET);
#else
static struct zzapp_coherent g_coh_store[ZZAPP_AS_SLOT_COUNT];
static struct zzapp_coherent *const g_coh = g_coh_store;
static uint64_t g_test_ticks;
#endif

_Static_assert(sizeof(struct zzapp_coherent) * ZZAPP_AS_SLOT_COUNT <= 0x1000u,
               "coherent slots exceed reserved region");

struct zzapp_host_slot {
    uint8_t *mem;
    uint32_t card;
    uint32_t owner;
    uint32_t state;
    uint32_t entry;
    int image_loaded;
    uint32_t image_end;
    uint32_t stack_bytes;
    uint64_t stop_mark;
    uint64_t fault_mark;
    uint32_t exit_code;
    uint32_t heap_off;
    uint32_t heap_end;
    uint32_t heap_bump;
    uint32_t stack_canary;
    int faulted;
    /* Core 1 only: keep the hot SVC watchdog state out of the uncached
     * lifecycle block. The timer IRQ samples it every 5 ms. */
    volatile uint32_t api_calls;
    volatile uint32_t in_api;
    volatile uint32_t service_due;
#if ZZAPP_PERF_COUNTERS
    uint8_t perf_svc_sampled;
#endif
    uint32_t seen_api_calls;
    uint64_t api_entered;
    uint64_t last_tick;
    volatile uint32_t watchdog_reason;
    uint32_t terminal_reason;
    volatile uint32_t stop_requested;
    volatile uint32_t stop_pending;
    uint32_t pending_fault;
    uint32_t pending_pc;
    uint32_t pending_sp;
    uint32_t pending_lr;
    uint32_t pending_far;
    uint32_t pending_fsr;
    uint32_t pending_cpsr;
    uint32_t stop_thread_state;
    uint32_t stop_wake_reason;
    uint32_t stop_in_api;
    uint32_t stop_parked;
    uint32_t stop_wait_registered;
    uint32_t stop_svc_number;
    uint32_t stop_diagnostics_valid;
    volatile uint32_t low_memory_pending;
    volatile uint32_t low_memory_crossings;
};

static struct zzapp_host_slot g_host[ZZAPP_AS_SLOT_COUNT];
static zzapp_proc_t g_proc[ZZAPP_AS_SLOT_COUNT];
static zzapp_kernel_t g_kernel;
static int g_cold_restarts;
static int g_finish_tlb_on_stop = 1;
static uint8_t g_api_parked[ZZAPP_AS_SLOT_COUNT];

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
#endif

static struct zzapp_coherent *coherent(uint32_t slot)
{
    return &g_coh[slot < ZZAPP_AS_SLOT_COUNT ? slot : 0U];
}

static struct zzapp_host_slot *host_slot(uint32_t slot)
{
    return &g_host[slot < ZZAPP_AS_SLOT_COUNT ? slot : 0U];
}

#ifndef ZZAPP_HOST_TEST
static uint8_t *proc_alias(uint32_t offset, uint32_t bytes);
#endif

zzapp_proc_t *zzapp_proc_slot(uint32_t slot)
{
    return slot < ZZAPP_AS_SLOT_COUNT ? &g_proc[slot] : NULL;
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
zzapp_arm_frame_t *zzapp_kernel_svc_return(zzapp_arm_frame_t *frame)
{
    int slot = zzapp_kernel_current_thread();

    if (slot >= 0 && slot < (int)ZZAPP_KERNEL_APP_SLOTS &&
        g_kernel.threads[slot].state == ZZAPP_THREAD_RUNNABLE &&
        !g_kernel.service_due)
        return frame;
    return zzapp_kernel_arch_dispatch(frame, ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0U);
}

int zzapp_kernel_svc_can_return(void)
{
    int slot = zzapp_kernel_current_thread();

    return slot >= 0 && slot < (int)ZZAPP_KERNEL_APP_SLOTS &&
           g_kernel.threads[slot].state == ZZAPP_THREAD_RUNNABLE &&
           !g_kernel.service_due;
}

void zzapp_kernel_switch_address_space(uint32_t slot)
{
#ifndef ZZAPP_HOST_TEST
    if (slot < ZZAPP_AS_SLOT_COUNT)
        zzapp_as_switch(&g_proc[slot].as);
#else
    (void)slot;
#endif
}

void zzapp_kernel_switch_kernel_address_space(void)
{
#ifndef ZZAPP_HOST_TEST
    zzapp_as_switch_kernel();
#endif
}


static uint32_t current_slot(void)
{
    return zzapp_services_current_slot();
}

static uint32_t select_teardown_slot(uint32_t slot)
{
    uint32_t selected = current_slot();

    zzapp_services_select_slot(slot);
    return selected;
}

static void restore_teardown_slot(uint32_t selected)
{
    zzapp_services_select_slot(selected);
}

int zzapp_proc_heap_grow(uint32_t bytes, void **old_break)
{
    uint32_t slot = current_slot();
    zzapp_proc_t *proc = &g_proc[slot];

    if (!bytes || bytes % ZZAPP_MEM_FRAME_BYTES ||
        proc->note_minor < 3u ||
        bytes > proc->heap_limit - proc->heap_break ||
        bytes > proc->memory.limit - proc->memory.committed)
        return ZZ_ENOMEM;
    if (old_break)
#ifdef ZZAPP_HOST_TEST
        *old_break = host_slot(slot)->mem + proc->heap_break;
#else
        *old_break = (void *)(uintptr_t)(ZZAPP_AS_WINDOW_BASE + proc->heap_break);
#endif
#ifndef ZZAPP_HOST_TEST
    if (zzapp_loader_heap_grow(bytes) != ZZ_OK)
        return ZZ_ENOMEM;
#endif
    proc->heap_break += bytes;
#ifdef ZZAPP_HOST_TEST
    proc->memory.committed += bytes;
#endif
    return ZZ_OK;
}

int zzapp_proc_heap_shrink(uint32_t bytes, void **new_break)
{
    uint32_t slot = current_slot();
    zzapp_proc_t *proc = &g_proc[slot];

    if (!bytes || bytes % ZZAPP_MEM_FRAME_BYTES || proc->note_minor < 3u ||
        bytes > proc->heap_break - proc->heap_start)
        return ZZ_EINVAL;
#ifndef ZZAPP_HOST_TEST
    if (zzapp_loader_heap_shrink(bytes) != ZZ_OK)
        return ZZ_EINVAL;
#endif
    proc->heap_break -= bytes;
#ifdef ZZAPP_HOST_TEST
    proc->memory.committed -= bytes;
#endif
    if (new_break)
#ifdef ZZAPP_HOST_TEST
        *new_break = host_slot(slot)->mem + proc->heap_break;
#else
        *new_break = (void *)(uintptr_t)(ZZAPP_AS_WINDOW_BASE + proc->heap_break);
#endif
    return ZZ_OK;
}

int zzapp_proc_heap_info(uint32_t *committed, uint32_t *limit,
                         uint32_t *pool_free)
{
    zzapp_proc_t *proc = &g_proc[current_slot()];

    if (!committed || !limit || !pool_free)
        return ZZ_EINVAL;
    *committed = proc->memory.committed;
    *limit = proc->memory.limit;
#ifndef ZZAPP_HOST_TEST
    *pool_free = zzapp_loader_pool_free();
#else
    *pool_free = 0u;
#endif
    return ZZ_OK;
}

#define ZZAPP_HOST_VISIBLE_LIMIT (16u << 20)
#define ZZAPP_HOST_VISIBLE_SLOTS 64u
struct zzapp_host_visible_block {
    void *ptr;
    uint32_t card;
    uint32_t alias;
    uint32_t bytes;
};
static struct zzapp_host_visible_block
    g_host_visible[ZZAPP_AS_SLOT_COUNT][ZZAPP_HOST_VISIBLE_SLOTS];
static volatile uint32_t g_host_visible_commit_active[ZZAPP_AS_SLOT_COUNT];
static uint32_t g_host_visible_used[ZZAPP_AS_SLOT_COUNT];
#ifndef ZZAPP_HOST_TEST
static int host_visible_alias_map(uint32_t slot, uint32_t card,
                                  uint32_t bytes, uint32_t align,
                                  uint32_t *alias)
{
    if (!alias || (card & 0xfffU) || !bytes || (bytes & 0xfffU) ||
        card + bytes < card || card + bytes > 0x40000000U)
        return -1;
    return zzapp_as_alias_map_aligned(&g_proc[slot].as, card, bytes, 1,
                                      align, alias);
}

static int host_visible_alias_unmap(uint32_t slot, uint32_t alias,
                                    uint32_t bytes)
{
    return zzapp_as_alias_unmap(&g_proc[slot].as, alias, bytes);
}
#endif


static void host_visible_release_all(void)
{
    struct zzapp_host_visible_block *blocks = g_host_visible[current_slot()];
    uint32_t i;

    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i) {
        if (!blocks[i].ptr)
            continue;
#ifndef ZZAPP_HOST_TEST
        (void)host_visible_alias_unmap(current_slot(), blocks[i].alias,
                                       blocks[i].bytes);
        (void)surface_allocator_free(blocks[i].card);
#else
        free(blocks[i].ptr);
#endif
        blocks[i].ptr = NULL;
    }
    g_host_visible_used[current_slot()] = 0;
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

static void life32(uint32_t off, uint32_t value)
{
    uint8_t *window = (uint8_t *)zzapp_services_window();
    if (window)
        zzapp_put_be32(window + ZZAPP_LIFE_OFF + off, value);
}

static uint32_t life_get32(uint32_t off)
{
    uint8_t *window = (uint8_t *)zzapp_services_window();
    return window ? zzapp_get_be32(window + ZZAPP_LIFE_OFF + off) : 0;
}

static void life_progress(uint64_t ticks)
{
    life32(ZZAPP_LIFE_PROGRESS_LO_OFF, (uint32_t)ticks);
    life32(ZZAPP_LIFE_PROGRESS_HI_OFF, (uint32_t)(ticks >> 32));
}

/* The selected service window is the lifecycle authority outside an app
 * context; an app context always resolves to its scheduler slot. */
static void publish_state(uint32_t state)
{
    uint32_t slot = current_slot();

    host_slot(slot)->state = state;
    coherent_publish();
    life32(ZZAPP_LIFE_STATE_OFF, state);
}

void zzapp_host_register_low_memory_callback(zzapp_mem_t *memory)
{
    if (zzapp_mem_set_low_memory_callback)
        zzapp_mem_set_low_memory_callback(memory, zzapp_host_low_memory, NULL);
}

void zzapp_host_reset_low_memory(uint32_t slot)
{
    struct zzapp_host_slot *host;

    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return;
    host = host_slot(slot);
    __atomic_store_n(&host->low_memory_pending, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&host->low_memory_crossings, 0u, __ATOMIC_RELAXED);
}

void zzapp_host_claimed(uint32_t slot)
{
    if (slot < ZZAPP_AS_SLOT_COUNT)
        host_slot(slot)->terminal_reason = ZZAPP_END_NONE;
}

void zzapp_loader_bound(uint32_t card, uint8_t *mem, uint32_t bytes,
                        uint32_t owner, uint32_t generation)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);
    struct zzapp_coherent *coh = coherent(slot);

    host->card = card;
    host->mem = mem;
    host->owner = owner;
    host->terminal_reason = ZZAPP_END_NONE;
    host->stop_requested = 0u;
    zzapp_host_reset_low_memory(slot);
    g_proc[slot].owner = owner;
    g_proc[slot].generation = generation;
    coh->running = 0;
    coh->in_api = 0;
    coh->fault = ZZAPP_FAULT_NONE;
    coh->arena_card = card;
    coh->arena_bytes = bytes;
    coh->owner = owner;
    coh->generation = generation;
    life32(ZZAPP_LIFE_MAGIC_OFF, ZZAPP_LIFE_MAGIC);
    life32(ZZAPP_LIFE_VERSION_OFF, ZZAPP_LIFE_VERSION);
    life32(ZZAPP_LIFE_GENERATION_OFF, generation);
    life32(ZZAPP_LIFE_FAULT_OFF, ZZAPP_FAULT_NONE);
    life32(ZZAPP_LIFE_PC_OFF, 0u);
    life32(ZZAPP_LIFE_SP_OFF, 0u);
    life32(ZZAPP_LIFE_LR_OFF, 0u);
    life32(ZZAPP_LIFE_FAR_OFF, 0u);
    life32(ZZAPP_LIFE_FSR_OFF, 0u);
    life32(ZZAPP_LIFE_CPSR_OFF, 0u);
    life32(ZZAPP_LIFE_EXIT_OFF, 0u);
    life32(ZZAPP_LIFE_LOW_MEMORY_CROSSINGS_OFF, 0u);
}

void zzapp_host_low_memory(void *context)
{
    uint32_t slot;

    (void)context;
    for (slot = 0u; slot < ZZAPP_AS_SLOT_COUNT; slot++) {
        struct zzapp_host_slot *host = host_slot(slot);

        if (!__atomic_load_n(&coherent(slot)->running, __ATOMIC_ACQUIRE))
            continue;
        __atomic_add_fetch(&host->low_memory_pending, 1u, __ATOMIC_RELAXED);
        zzapp_services_set_low_memory_crossings(
            slot, __atomic_add_fetch(&host->low_memory_crossings, 1u,
                                     __ATOMIC_RELAXED));
    }
}

int zzapp_host_take_low_memory_event(uint32_t slot)
{
    struct zzapp_host_slot *host;
    uint32_t pending;

    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return 0;
    host = host_slot(slot);
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
void zzapp_loader_unbound(void)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);
    struct zzapp_coherent *coh = coherent(slot);
    uint32_t state = host->state;
    uint64_t fault_mark = host->fault_mark;
    int faulted = host->faulted;

    host_visible_release_all();
    if (zzapp_api_reclaim_resources)
        zzapp_api_reclaim_resources();
    zzapp_kernel_stop_app(&g_kernel, slot);
    memset(host, 0, sizeof(*host));
    host->state = state;
    host->fault_mark = fault_mark;
    host->faulted = faulted;
    memset(&g_proc[slot], 0, sizeof(g_proc[slot]));
    coh->arena_card = 0;
    coh->arena_bytes = 0;
    coh->owner = 0;
}

uint32_t zzapp_loader_generation(void) { return coherent(current_slot())->generation; }
uint32_t zzapp_loader_state(void) { return host_slot(current_slot())->state; }
void zzapp_loader_set_state(uint32_t state) { publish_state(state); }
uint32_t zzapp_loader_reset_requested(void) { return coherent(current_slot())->reset_request; }
int zzapp_loader_have_image(void) { return host_slot(current_slot())->image_loaded; }

int zzapp_loader_set_image(uint32_t entry, uint32_t image_end,
                           uint32_t stack_bytes, uint32_t note_flags,
                           uint32_t note_minor, uint32_t min_heap_bytes)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);
    zzapp_proc_t *proc = &g_proc[slot];
    uint32_t image_bytes = (image_end + ZZAPP_MEM_FRAME_BYTES - 1u) &
                           ~(ZZAPP_MEM_FRAME_BYTES - 1u);
    uint32_t stack_commit = (stack_bytes + ZZAPP_MEM_FRAME_BYTES - 1u) &
                            ~(ZZAPP_MEM_FRAME_BYTES - 1u);
    uint32_t commit = note_minor < 3u ? ZZAPP_ARENA_BYTES :
                      image_bytes + stack_commit + min_heap_bytes;
    uint32_t limit = note_minor < 3u ? ZZAPP_ARENA_BYTES :
                     ZZAPP_MEM_PRIVATE_WINDOW_BYTES;
    uint32_t requested_limit = zzapp_services_memory_limit();

    if (note_minor >= 3u && requested_limit != 0u &&
        requested_limit < limit)
        limit = requested_limit;
    if (commit > limit)
        return ZZ_ENOMEM;
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
    proc->stack_base = note_minor < 3u ?
                       ZZAPP_ARENA_BYTES - stack_commit :
                       ZZAPP_MEM_PRIVATE_WINDOW_BYTES - stack_commit;
    proc->heap_limit = note_minor < 3u ? proc->stack_base :
                       ZZAPP_AS_ALIAS_BASE - ZZAPP_AS_WINDOW_BASE;
    proc->heap_frame_count = note_minor < 3u ?
        (proc->stack_base - proc->heap_base) / ZZAPP_MEM_FRAME_BYTES :
        min_heap_bytes / ZZAPP_MEM_FRAME_BYTES;
#endif
    proc->note_minor = note_minor;
    proc->memory.committed = commit;
    proc->memory.limit = limit;
    life32(ZZAPP_LIFE_FLAGS_OFF,
           note_flags & (ZZAPP_F_DISPLAY | ZZAPP_F_AUDIO | ZZAPP_F_GL));
    return ZZ_OK;
}

uint32_t zzapp_current_generation(void) { return coherent(current_slot())->generation; }
uint32_t zzapp_runtime_state(void) { return host_slot(current_slot())->state; }
uint32_t zzapp_runtime_caps(void) { return ZZAPP_CAP_Z3 | ZZAPP_CAP_DISPLAY | ZZAPP_CAP_AUDIO | ZZAPP_CAP_GL | ZZAPP_CAP_CLUT8; }
int zzapp_host_faulted(void) { return host_slot(current_slot())->faulted; }

void zzapp_note_progress(void)
{
    uint64_t now = now_ticks();
    coherent(current_slot())->progress = now;
    life_progress(now);
}

void zzapp_host_latch_stop(uint32_t slot)
{
    if (slot < ZZAPP_AS_SLOT_COUNT)
        host_slot(slot)->stop_requested = 1u;
}

void zzapp_host_clear_stop_requested(uint32_t slot)
{
    if (slot < ZZAPP_AS_SLOT_COUNT)
        host_slot(slot)->stop_requested = 0u;
}

int zzapp_host_stop_requested(void)
{
    return host_slot(current_slot())->stop_requested != 0u;
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
static uint32_t terminal_reason(const struct zzapp_host_slot *host,
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
    struct zzapp_host_slot *host = host_slot(slot);
    struct zzapp_coherent *coh = coherent(slot);

    if (host->stop_diagnostics_valid)
        return;
    host->stop_thread_state = (uint32_t)g_kernel.threads[slot].state;
    host->stop_wake_reason = (uint32_t)g_kernel.threads[slot].wake_reason;
    host->stop_in_api = coh->in_api;
    host->stop_parked = g_api_parked[slot];
    host->stop_wait_registered =
        (uint32_t)zzapp_services_wait_registered(slot);
#ifndef ZZAPP_HOST_TEST
    host->stop_svc_number = zzapp_svc_last_number[slot];
#else
    host->stop_svc_number = 0u;
#endif
    host->stop_diagnostics_valid = 1u;
}

static void request_stop(uint32_t slot, uint32_t fault, uint32_t pc,
                         uint32_t sp, uint32_t lr, uint32_t far,
                         uint32_t fsr, uint32_t cpsr)
{
    struct zzapp_host_slot *host = host_slot(slot);
    struct zzapp_coherent *coh = coherent(slot);
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
    struct zzapp_host_slot *host = host_slot(slot);
    struct zzapp_coherent *coh = coherent(slot);
    uint32_t selected;

    (void)cpsr;
    if (!claim_fault(slot))
        return;
    selected = select_teardown_slot(slot);
    coh->running = 0;
    coh->fault = fault;
    life32(ZZAPP_LIFE_FAULT_OFF, fault);
    life32(ZZAPP_LIFE_PC_OFF, pc);
    life32(ZZAPP_LIFE_SP_OFF, sp);
    snapshot_stop_diagnostics(slot);
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    zzapp_svc_counters_report(slot);
    if (finish_tlb)
        zzapp_kernel_perf_finish_slot(&g_kernel, slot);
    zzapp_kernel_perf_request_report(&g_kernel, slot, 1);
#endif
    life32(ZZAPP_LIFE_LR_OFF, lr);
    life32(ZZAPP_LIFE_FAR_OFF, far);
    life32(ZZAPP_LIFE_FSR_OFF, fsr);
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
    publish_state(ZZAPP_ST_FAULTED);
    host->fault_mark = now_ticks();
    zzapp_services_set_terminal(terminal_reason(host, fault));
    zzapp_services_on_app_end();
    zzapp_minigl_reclaim(coh->generation);
#ifdef ZZAPP_HOST_TEST
    if (coh->reset_request)
        zzapp_services_on_reset();
#endif
    if (zzapp_api_poison_resources)
        zzapp_api_poison_resources();
    zzapp_loader_release();
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
    struct zzapp_host_slot *host = host_slot(slot);
    uint32_t expected = 1u;

    if (!__atomic_compare_exchange_n(&host->stop_pending, &expected, 0u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    force_stop_slot(slot, host->pending_fault, host->pending_pc,
                    host->pending_sp, host->pending_lr, host->pending_far,
                    host->pending_fsr, host->pending_cpsr, 1);
}


static void maybe_expire_fault(uint64_t now)
{
    struct zzapp_host_slot *host = host_slot(current_slot());

    if (host->state == ZZAPP_ST_FAULTED && now > host->fault_mark &&
        ticks_to_us(now - host->fault_mark) >= ZZAPP_WATCHDOG_US)
        publish_state(ZZAPP_ST_IDLE);
}

static void begin_tick_observation(void)
{
    uint32_t slot = current_slot();
    struct zzapp_coherent *coh = coherent(slot);
    struct zzapp_host_slot *host = host_slot(slot);

    host->in_api = 0u;
    host->service_due = 0u;
    host->seen_api_calls = host->api_calls;
    host->last_tick = now_ticks();
    coh->in_api = 0u;
    coherent(0)->core_heartbeat = host->last_tick;
}

/* The private timer may interrupt an app API or system service pass. It only
 * updates scheduler-visible state; teardown and service code stay on the
 * system thread. */
static void service_tick_irq(uint64_t now)
{
    uint32_t slot;
    struct zzapp_host_slot *host;
    struct zzapp_coherent *coh;
    uint32_t calls;

    coherent(0)->core_heartbeat = now;
    zzapp_kernel_service_due(&g_kernel);
    slot = (uint32_t)zzapp_kernel_current_thread();
#ifdef ZZAPP_HOST_TEST
    if (slot >= ZZAPP_AS_SLOT_COUNT)
        slot = current_slot();
#else
    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return;
#endif
    host = host_slot(slot);
    coh = coherent(slot);
    coh->tick_seen = now;
    calls = host->api_calls;
    coh->in_api = host->in_api;
    if (calls != host->seen_api_calls) {
        host->seen_api_calls = calls;
        coh->progress = now;
        if (host->in_api != 0u)
            host->api_entered = host->last_tick;
    }
    host->last_tick = now;
    /* ZZRun can set this host-visible word before its best-effort APP_STOP
     * reaches firmware.  Sample it at the scheduler tick, never per SVC. */
    if (life_get32(ZZAPP_LIFE_STOP_OFF) != 0u)
        zzapp_host_latch_stop(slot);
    host->service_due = 1u;
    if (g_host_visible_commit_active[slot] || !coh->running)
        return;
    if (coh->reset_request || host->stop_requested != 0u) {
        if (!host->stop_mark)
            host->stop_mark = now;
        if (coh->reset_request ||
            ticks_to_us(now - host->stop_mark) >= ZZAPP_STOP_GRACE_US)
            request_stop(slot, ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
        return;
    }
    if (host->in_api == 0u &&
        ticks_to_us(now - coh->progress) >= ZZAPP_APP_STALL_US) {
        host->watchdog_reason = 1u;
        request_stop(slot, ZZAPP_FAULT_WATCHDOG, 0, 0, 0, 0, 0, 0);
    } else if (host->in_api != 0u && !g_api_parked[slot] &&
               ticks_to_us(now - host->api_entered) >= ZZAPP_API_STUCK_US) {
        host->watchdog_reason = 2u;
        request_stop(slot, ZZAPP_FAULT_WATCHDOG, 0, 0, 0, 0, 0, 0);
    }
}


static void check_stack_canary(void);

static void service_watch_slot(uint64_t now)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);
    struct zzapp_coherent *coh = coherent(slot);

    check_stack_canary();
    /* The worker runs once per service tick and visits every slot.  ZZRun can
     * write this host-visible word even when its best-effort APP_STOP is
     * dropped, so sample every valid slot here rather than only the app that
     * received the timer interrupt. */
    if (life_get32(ZZAPP_LIFE_STOP_OFF) != 0u)
        zzapp_host_latch_stop(slot);
    if (g_host_visible_commit_active[slot])
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
    struct zzapp_host_slot *host = host_slot(current_slot());
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

    while (1) {
#ifndef ZZAPP_HOST_TEST
        uint32_t cpsr = api_lock_irq_save();
#endif

        if (zzapp_kernel_api_lock(&g_kernel, slot) == 0) {
#ifndef ZZAPP_HOST_TEST
            api_lock_irq_restore(cpsr);
#endif
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
    }
}

static void resume_parked_api(uint32_t slot)
{
    struct zzapp_host_slot *host = host_slot(slot);

    host->api_entered = now_ticks();
    host->seen_api_calls = host->api_calls;
    g_api_parked[slot] = 0u;
}


void zzapp_host_yield(void)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);

    zzapp_note_progress();
    host->service_due = 1u;
    zzapp_kernel_service_due(&g_kernel);
    /* Yield is the API body's safe interleaving point.  Never preempt a
     * non-yielding body: it may hold service-visible state on its stack. */
    if (zzapp_kernel_current_thread() < 0 || host->in_api == 0u ||
        g_kernel.api_lock_owner != (int)slot ||
        g_kernel.api_lock_depth != 1U)
        return;
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    if (host->perf_svc_sampled)
        zzapp_kernel_perf_service_end(&g_kernel, ZZAPP_PERF_SERVICE_SVC,
                                      scheduler_perf_stamp());
#endif
    g_api_parked[slot] = 1u;
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
    struct zzapp_host_slot *host = host_slot(slot);

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
    g_api_parked[slot] = zzapp_services_wait_registered(slot) ? 1u : 0u;
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    if (g_kernel.api_lock_depth == 1U && host->perf_svc_sampled)
        zzapp_kernel_perf_service_end(&g_kernel, ZZAPP_PERF_SERVICE_SVC,
                                      scheduler_perf_stamp());
#endif
    if (!g_api_parked[slot]) {
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
    if (!g_api_parked[slot]) {
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
    g_api_parked[slot] = 1u;
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
    struct zzapp_host_slot *host = host_slot(slot);

    g_api_parked[slot] = 0u;
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
    host->api_calls++;
    host->in_api++;
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
    struct zzapp_host_slot *host = host_slot(slot);

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
    zzapp_proc_t *proc = &g_proc[current_slot()];
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
    zzapp_proc_t *proc = &g_proc[current_slot()];
    uint32_t need;
    uint32_t have;

    if (end_offset <= proc->heap_base)
        return ZZ_OK;
    need = (end_offset - proc->heap_base + ZZAPP_MEM_FRAME_BYTES - 1u) >> 20;
    have = proc->heap_frame_count;
    if (need <= have)
        return ZZ_OK;
    if (need > ZZAPP_MEM_MAX_FRAMES)
        return ZZ_ENOMEM;
#ifndef ZZAPP_HOST_TEST
    if (zzapp_loader_heap_grow((need - have) * ZZAPP_MEM_FRAME_BYTES) != ZZ_OK)
        return ZZ_ENOMEM;
#else
    proc->heap_frame_count = need;
    if (proc->heap_break < proc->heap_base + need * ZZAPP_MEM_FRAME_BYTES)
        proc->heap_break = proc->heap_base + need * ZZAPP_MEM_FRAME_BYTES;
#endif
    return ZZ_OK;
}

/* One committed frame only. A span that crosses a frame is copied in pieces. */
static uint8_t *argv_frame_alias(uint32_t offset, uint32_t bytes)
{
    zzapp_proc_t *proc = &g_proc[current_slot()];
    uint32_t in_frame = offset & (ZZAPP_MEM_FRAME_BYTES - 1u);

    if (!bytes || bytes > ZZAPP_MEM_FRAME_BYTES - in_frame)
        return NULL;
#ifdef ZZAPP_HOST_TEST
    {
        struct zzapp_host_slot *host = host_slot(current_slot());
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
            return ZZ_ENOMEM;
        if (from)
            memcpy(dst, from, n);
        else
            memset(dst, 0, n);
        offset += n;
        if (from)
            from += n;
        bytes -= n;
    }
    return ZZ_OK;
}

static int copy_argv(uint32_t argc, char ***argv_out)
{
    struct zzapp_host_slot *host = host_slot(current_slot());
    uint8_t *window = (uint8_t *)zzapp_services_window();
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
        return ZZ_EINVAL;
    if (argc && !window)
        return ZZ_EIO;
    for (i = 0; i < argc; i++) {
        uint32_t off = zzapp_get_be32(window + ZZAPP_ARGV_OFF +
                                      ZZAPP_ARGV_OFFS_OFF + 4u * i);
        uint32_t n = 0;

        if (off < ZZAPP_ARGV_OFF || off >= ZZAPP_ARGV_OFF + ZZAPP_ARGV_BYTES)
            return ZZ_EINVAL;
        while (off + n < ZZAPP_ARGV_OFF + ZZAPP_ARGV_BYTES && window[off + n])
            n++;
        if (off + n >= ZZAPP_ARGV_OFF + ZZAPP_ARGV_BYTES ||
            n >= ZZAPP_ARGV_STR_MAX)
            return ZZ_EINVAL;
        string_at[i] = off;
        string_n[i] = n;
        total += n + 1u;
    }
    total = (total + 7u) & ~7u;
    if (host->heap_bump + total > host->heap_end)
        return ZZ_ENOMEM;
    if (commit_argv_frames(host->heap_bump + total) != ZZ_OK)
        return ZZ_ENOMEM;
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

        if (write_argv_bytes(value_off, window + string_at[i], n) != ZZ_OK ||
            write_argv_bytes(value_off + n, NULL, 1u) != ZZ_OK ||
            write_argv_bytes(argv_offset + i * ptr_bytes, &value,
                             ptr_bytes) != ZZ_OK)
            return ZZ_ENOMEM;
        host->heap_bump += n + 1u;
    }
    if (write_argv_bytes(argv_offset + argc * ptr_bytes, NULL, ptr_bytes) != ZZ_OK)
        return ZZ_ENOMEM;
#ifdef ZZAPP_HOST_TEST
    *argv_out = (char **)(host->mem + argv_offset);
#else
    *argv_out = (char **)(uintptr_t)(ZZAPP_AS_WINDOW_BASE + argv_offset);
#endif
    return ZZ_OK;
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

static void start_failed(uint32_t slot, int status)
{
    uint32_t selected = select_teardown_slot(slot);

    zzapp_services_start_entered(status);
    zzapp_loader_release();
    restore_teardown_slot(selected);
}

static void run_app(uint32_t argc)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);
    struct zzapp_coherent *coh = coherent(slot);
    char **argv = NULL;
    const zzapp_api *app_api;
    int rc;
    zzapp_proc_t *proc = &g_proc[slot];
#ifdef ZZAPP_HOST_TEST
    uint32_t teardown_selected;
#endif

    if (!host->image_loaded || !host->mem) { start_failed(slot, ZZ_EINVAL); return; }
    host->heap_off = (host->image_end + 7u) & ~7u;
#ifdef ZZAPP_HOST_TEST
    host->heap_end = ZZAPP_ARENA_BYTES - host->stack_bytes;
#else
    host->heap_end = proc->heap_limit;
#endif
    host->heap_bump = host->heap_off;
    if (host->heap_end <= host->heap_off) { start_failed(slot, ZZ_ENOMEM); return; }
    rc = copy_argv(argc, &argv);
    if (rc != ZZ_OK) { start_failed(slot, rc); return; }
    host->heap_bump = (host->heap_bump + 7u) & ~7u;
    proc->heap_start = host->heap_bump;
    proc->heap_break = proc->heap_base +
                       proc->heap_frame_count * ZZAPP_MEM_FRAME_BYTES;
    proc->heap_limit = host->heap_end;
#ifndef ZZAPP_HOST_TEST
    if (!protect_firmware_text()) {
        start_failed(slot, ZZ_EIO);
        return;
    }
    {
        uint32_t veneer_alias;
        uint32_t api_alias;
        uint32_t gl_alias;
        zzapp_gl *gl;

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
            start_failed(slot, ZZ_EIO);
            return;
        }
        app_api = (const zzapp_api *)(uintptr_t)api_alias;
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
        if (!canary) { start_failed(slot, ZZ_ENOMEM); return; }
        memset(canary, 0xA5, ZZAPP_CANARY_BYTES);
    }
#endif
    host->stop_mark = 0;
    host->fault_mark = 0;
    host->faulted = 0;
    host->stop_diagnostics_valid = 0u;
    host->exit_code = 0;
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    zzapp_svc_counters_reset(slot);
#endif
    coh->heartbeat_value = life_get32(ZZAPP_LIFE_HEARTBEAT_OFF);
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
        start_failed(slot, ZZ_EINVAL);
        return;
    }
    coh->running = 1;
    publish_state(ZZAPP_ST_RUNNING);
    zzapp_services_start_entered(ZZ_OK);
    (void)zzapp_kernel_start_app(&g_kernel, slot);
#ifdef ZZAPP_HOST_TEST
    zzapp_kernel_set_current_thread((int)slot);
    zzapp_private_timer_enable();
    if (g_test_entry)
        g_test_entry(zzapp_api_table(), (int)argc, argv);
    zzapp_private_timer_disable();
    zzapp_kernel_set_current_thread(-1);
    zzapp_worker_hook();
    teardown_selected = select_teardown_slot(slot);
    coh->running = 0;
    host->watchdog_reason = 0u;
    if (!host->faulted) {
        if (coh->reset_request) {
            zzapp_services_set_terminal(ZZAPP_END_AMIGA_RESET);
            zzapp_services_on_reset();
        } else {
            life32(ZZAPP_LIFE_EXIT_OFF, host->exit_code);
            zzapp_services_set_terminal(host->terminal_reason ?
                                        host->terminal_reason : ZZAPP_END_EXIT);
            zzapp_services_on_app_end();
        }
        zzapp_minigl_reclaim(coh->generation);
        publish_state(ZZAPP_ST_IDLE);
        zzapp_loader_release();
    }
    restore_teardown_slot(teardown_selected);
#else
    zzapp_kernel_set_current_thread(-1);
    zzapp_private_timer_enable();
#endif
}

static void service_reset(uint32_t slot)
{
    struct zzapp_host_slot *host = host_slot(slot);
    struct zzapp_coherent *coh = coherent(slot);
    uint32_t selected;

    if (!coh->reset_request || g_host_visible_commit_active[slot])
        return;
    selected = select_teardown_slot(slot);
    if (host->state == ZZAPP_ST_LOADING || host->state == ZZAPP_ST_STARTING) {
        zzapp_services_cancel_start(ZZ_EIO);
        (void)zzapp_loader_abort();
    } else if (coh->running) {
        force_stop(ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
    }
    if (host->state == ZZAPP_ST_FAULTED) {
        zzapp_loader_release();
        publish_state(ZZAPP_ST_IDLE);
    }
    zzapp_services_on_reset();
    zzapp_host_clear_stop_requested(slot);
    host->stop_mark = 0;
    host->fault_mark = 0;
    coherent_barrier();
    coh->reset_ack = 1;
    coh->reset_request = 0;
    coherent_publish();
    restore_teardown_slot(selected);
}

void zzapp_worker_hook(void)
{
    uint32_t selected = current_slot();
    uint32_t slot;
    struct zzapp_host_slot *host;
    uint32_t argc = 0;
    uint64_t now = now_ticks();

    for (slot = 0; slot < ZZAPP_AS_SLOT_COUNT; ++slot) {
        if (coherent(slot)->reset_request && coherent(slot)->running &&
            !g_host_visible_commit_active[slot])
            request_stop(slot, ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
        zzapp_services_select_slot(slot);
        complete_pending_stop(slot);
    }
    for (slot = 0; slot < ZZAPP_AS_SLOT_COUNT; ++slot) {
        if (!coherent(slot)->reset_request)
            continue;
        zzapp_services_select_slot(slot);
        service_reset(slot);
        /* service_reset returns only after this slot's teardown has no
         * worker task in flight, so a pending core-1 cache clean is safe. */
        scheduler_core1_service_clean_request();
    }
    for (slot = 0; slot < ZZAPP_AS_SLOT_COUNT; ++slot) {
        zzapp_services_select_slot(slot);
        service_watch_slot(now);
        complete_pending_stop(slot);
        if (host_slot(slot)->state == ZZAPP_ST_LOADING && stop_requested()) {
            zzapp_services_cancel_start(ZZ_EIO);
            (void)zzapp_loader_abort();
        }
    }
    zzapp_services_select_slot(selected);
    if (coherent(selected)->reset_request)
        return;
    zzapp_services_poll();
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    zzapp_kernel_perf_switch_finished(&g_kernel);
    (void)zzapp_kernel_perf_emit_reports(&g_kernel);
#endif
#ifdef ZZAPP_HOST_TEST
    scheduler_core1_service_from_app();
#endif
    for (slot = 0U; slot < ZZAPP_AS_SLOT_COUNT; ++slot) {
        zzapp_services_select_slot(slot);
        host = host_slot(slot);
        if (host->state == ZZAPP_ST_LOADING &&
            life_get32(ZZAPP_LIFE_STOP_OFF) != 0u)
            zzapp_host_latch_stop(slot);
        if (host->state == ZZAPP_ST_LOADING && stop_requested()) {
            zzapp_services_cancel_start(ZZ_EIO);
            (void)zzapp_loader_abort();
            continue;
        }
        argc = 0U;
        if (host->state == ZZAPP_ST_LOADING &&
            zzapp_services_take_start(&argc)) {
            publish_state(ZZAPP_ST_STARTING);
            run_app(argc);
        }
    }
    zzapp_services_select_slot(selected);
}

int zzapp_fault_is_app(uint32_t pc, uint32_t abort_lr, uint32_t spsr)
{
    uint32_t slot = current_slot();

    /* An API body runs in SYS, and SVC entry is still SVC, while the app
     * stays current. Only a fault that interrupted user mode is the app's. */
    if ((spsr & 0x1fU) != ZZAPP_KERNEL_APP_CPSR)
        return 0;
#ifdef ZZAPP_HOST_TEST
    struct zzapp_host_slot *host = host_slot(slot);
    uint32_t end = host->card + ZZAPP_ARENA_BYTES;

    return coherent(slot)->running && host->card && end > host->card &&
           ((pc >= host->card && pc < end) ||
            (abort_lr >= host->card && abort_lr < end));
#else
    (void)pc;
    (void)abort_lr;
    return coherent(slot)->running && zzapp_kernel_current_thread() >= 0;
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
    uint32_t selected = zzapp_services_current_slot();
    int restart = 0;
    uint32_t restart_slot = 0U;
    uint64_t restart_age = 0U;
    uint32_t restart_in_api = 0U;
    zzapp_services_expire_claims(ticks_to_us(now_ticks()));
    uint64_t core_tick_age = core1_stamp_age_us(&coherent(0)->core_heartbeat);

    for (slot = 0U; slot < ZZAPP_AS_SLOT_COUNT; ++slot) {
        struct zzapp_host_slot *host = host_slot(slot);
        struct zzapp_coherent *coh = coherent(slot);
        uint32_t running = coh->running;
        uint64_t age;
        uint64_t now;
        uint32_t heartbeat;

        zzapp_services_select_slot(slot);
        coherent_barrier();
        age = core1_stamp_age_us(&coh->progress);
        now = now_ticks();
        heartbeat = life_get32(ZZAPP_LIFE_HEARTBEAT_OFF);
        maybe_expire_fault(now);
        if (heartbeat != coh->heartbeat_value) {
            coh->heartbeat_value = heartbeat;
            coh->heartbeat_seen = now;
        }
        if (running && host->state == ZZAPP_ST_RUNNING &&
            core1_stamp_age_us(&coh->heartbeat_seen) >= ZZAPP_LEASE_US) {
            host->terminal_reason = ZZAPP_END_LEASE;
            zzapp_host_latch_stop(slot);
            life32(ZZAPP_LIFE_STOP_OFF, 1u);
        }
        /* One stale slot restarts the shared worker once. Clearing running
         * here would hide the other slots from the retirement below. */
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
        printf("[app] watchdog restart: slot %lu no progress for %lu ms, "
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
    zzapp_services_select_slot(selected);

}

#ifdef ZZAPP_HOST_TEST
void __attribute__((weak)) zzapp_recover_external_locks(void)
{
}
#endif
void zzapp_recover_halted_core1(void)
{
    zzapp_kernel_t *kernel = zzapp_runtime_kernel();
    uint32_t slot;

    /* Every lock core 1 could hold, before any teardown acquires one.
     * Resetting the kernel lock wakes its waiters, so contexts are
     * stopped after that and before mappings are released. */
    zzapp_recover_external_locks();
    zzapp_kernel_reset_lock(kernel);
    if (kernel) {
        for (slot = 0U; slot < ZZAPP_KERNEL_APP_SLOTS; slot++) {
            zzapp_kernel_stop_app(kernel, slot);
            memset(&kernel->threads[slot].context, 0,
                   sizeof(kernel->threads[slot].context));
        }
        zzapp_kernel_reset_slice(kernel);
    }
    zzapp_kernel_set_current_thread(-1);
    zzapp_on_core1_restart();
}

void zzapp_on_core1_restart(void)
{
    uint32_t slot;
    uint32_t selected = zzapp_services_current_slot();

    g_finish_tlb_on_stop = 0;
    for (slot = 0U; slot < ZZAPP_AS_SLOT_COUNT; ++slot) {
        zzapp_services_select_slot(slot);
        if (coherent(slot)->running) {
            host_slot(slot)->terminal_reason = ZZAPP_END_CORE_RESTART;
            force_stop(ZZAPP_FAULT_WATCHDOG_RESTART, 0, 0, 0, 0, 0, 0);
        } else if (zzapp_services_state() != ZZAPP_ST_IDLE) {
            zzapp_services_set_terminal(ZZAPP_END_CORE_RESTART);
            if (zzapp_loader_abort() != ZZ_OK)
                zzapp_loader_release();
            zzapp_services_on_reset();
        }
    }
    g_finish_tlb_on_stop = 1;
    zzapp_services_select_slot(selected);
    g_cold_restarts++;
}

/* A running app stops itself within one private-timer tick and the worker
 * acks after its teardown.  Bounded well under the 2 s API-stall limit. */
#define ZZAPP_RESET_ACK_WAIT_US 1000000u

static void request_slot_reset(uint32_t slot)
{
    coherent(slot)->reset_ack = 0;
    coherent(slot)->reset_request = 1;
    host_slot(slot)->terminal_reason = ZZAPP_END_AMIGA_RESET;
    zzapp_host_latch_stop(slot);
}

void zzapp_on_amiga_reset(void)
{
    uint32_t slot;
    uint32_t selected = zzapp_services_current_slot();

    coherent_barrier();
    for (slot = 0U; slot < ZZAPP_AS_SLOT_COUNT; ++slot) {
        request_slot_reset(slot);
    }
    coherent_publish();
    wake_core1();
#ifndef ZZAPP_HOST_TEST
    for (slot = 0U; slot < ZZAPP_AS_SLOT_COUNT; ++slot) {
        uint64_t start;

        if (!coherent(slot)->running)
            continue;
        start = now_ticks();
        while (!coherent(slot)->reset_ack &&
               ticks_to_us(now_ticks() - start) < ZZAPP_RESET_ACK_WAIT_US)
            usleep(100u);
        if (!coherent(slot)->reset_ack)
            printf("[app] reset: core 1 did not stop slot %lu within 1 s\r\n",
                   (unsigned long)slot);
    }
#endif
    for (slot = 0U; slot < ZZAPP_AS_SLOT_COUNT; ++slot) {
        zzapp_services_select_slot(slot);
        if (zzapp_services_state() != ZZAPP_ST_CLAIMED)
            continue;
        zzapp_services_set_terminal(ZZAPP_END_AMIGA_RESET);
        zzapp_services_on_reset();
    }
    zzapp_services_select_slot(selected);
}

void zzapp_status_observed(void)
{
    if (host_slot(current_slot())->state == ZZAPP_ST_FAULTED)
        publish_state(ZZAPP_ST_IDLE);
}

int zzapp_flip_ack_is_current(uint32_t gen)
{
    return life_get32(ZZAPP_LIFE_FLIP_ACK_OFF) == gen;
}

void zzapp_boot_init(void)
{
    memset((void *)g_coh, 0, sizeof(*g_coh) * ZZAPP_AS_SLOT_COUNT);
    memset(g_host, 0, sizeof(g_host));
    memset(g_proc, 0, sizeof(g_proc));
    memset(g_api_parked, 0, sizeof(g_api_parked));
    zzapp_kernel_init(&g_kernel);
    zzapp_loader_memory_init();
    g_cold_restarts = 0;
    g_kernel.slice_started_ms = zzapp_kernel_now_ms();
    g_kernel.slice_valid = 1U;
#ifdef ZZAPP_HOST_TEST
    g_test_service_hook = NULL;
    g_test_service_calls = 0;
    memset(g_test_wait_registered, 0, sizeof(g_test_wait_registered));
    g_test_time_hook = NULL;
#endif
}

void zzapp_host_exit(int code)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);

    host->exit_code = (uint32_t)code;
#if ZZAPP_PERF_COUNTERS && !defined(ZZAPP_HOST_TEST)
    zzapp_svc_counters_report(slot);
    zzapp_kernel_perf_finish_slot(&g_kernel, slot);
    zzapp_kernel_perf_request_report(&g_kernel, slot, 1);
#endif
#ifndef ZZAPP_HOST_TEST
    struct zzapp_coherent *coh = coherent(slot);

    if (zzapp_kernel_current_thread() >= 0) {
        zzapp_kernel_stop_app(&g_kernel, slot);
        coh->running = 0;
        if (coh->reset_request) {
            zzapp_services_set_terminal(ZZAPP_END_AMIGA_RESET);
            zzapp_services_on_reset();
        } else {
            life32(ZZAPP_LIFE_EXIT_OFF, host->exit_code);
            zzapp_services_set_terminal(host->terminal_reason ?
                                        host->terminal_reason : ZZAPP_END_EXIT);
            zzapp_services_on_app_end();
        }
        zzapp_minigl_reclaim(coh->generation);
        publish_state(ZZAPP_ST_IDLE);
        zzapp_loader_release();
        zzapp_kernel_service_due(&g_kernel);
        __asm__ __volatile__("svc #0" ::: "memory");
        for (;;)
            __asm__ __volatile__("wfe" ::: "memory");
    }
#endif
}

void zzapp_host_commit_begin(void)
{
    g_host_visible_commit_active[current_slot()] = 1u;
}

void zzapp_host_commit_end(void)
{
    g_host_visible_commit_active[current_slot()] = 0u;
}

void *zzapp_host_alloc(uint32_t bytes, uint32_t align, uint32_t flags)
{
    uint32_t slot = current_slot();
    struct zzapp_host_visible_block *blocks = g_host_visible[slot];
    uint32_t i;

    if ((flags & ~(ZZ_ALLOC_UNCACHED | ZZ_ALLOC_HOST_VISIBLE)) != 0u ||
        !bytes || align < 8u || (align & (align - 1u)) != 0u ||
        bytes > UINT32_MAX - 0xfffU)
        return NULL;
    bytes = (bytes + 0xfffU) & ~0xfffU;
    if ((flags & ZZ_ALLOC_HOST_VISIBLE) == 0u ||
        bytes > ZZAPP_HOST_VISIBLE_LIMIT - g_host_visible_used[slot])
        return NULL;
    if (align < 4096u)
        align = 4096u;
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
            g_host_visible_commit_active[slot] = 0u;
            return NULL;
        }
        ptr = (void *)(uintptr_t)alias;
#else
        ptr = malloc(bytes);
        card = 0x033f0000u + g_host_visible_used[slot];
        alias = 0u;
        extern void zzapp_test_host_visible_allocated(void) __attribute__((weak));
        if (zzapp_test_host_visible_allocated)
            zzapp_test_host_visible_allocated();
#endif
        if (!ptr) {
            g_host_visible_commit_active[slot] = 0u;
            return NULL;
        }
        blocks[i].ptr = ptr;
        blocks[i].card = card;
        blocks[i].alias = alias;
        blocks[i].bytes = bytes;
        g_host_visible_used[slot] += bytes;
        g_host_visible_commit_active[slot] = 0u;
        return ptr;
    }
    return NULL;
}

void zzapp_host_free(void *p)
{
    uint32_t slot = current_slot();
    struct zzapp_host_visible_block *blocks = g_host_visible[slot];
    uint32_t i;

    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i) {
        if (blocks[i].ptr != p)
            continue;
        g_host_visible_commit_active[slot] = 1u;
#ifndef ZZAPP_HOST_TEST
        (void)host_visible_alias_unmap(slot, blocks[i].alias, blocks[i].bytes);
        (void)surface_allocator_free(blocks[i].card);
#else
        free(p);
#endif
        g_host_visible_used[slot] -= blocks[i].bytes;
        blocks[i].ptr = NULL;
        g_host_visible_commit_active[slot] = 0u;
        return;
    }
}

void zzapp_host_heap(void **base, uint32_t *bytes)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);
    zzapp_proc_t *proc = &g_proc[slot];

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

int zzapp_host_address(const void *p, uint32_t *host_offset)
{
    struct zzapp_host_visible_block *blocks = g_host_visible[current_slot()];
    uint32_t i;

    if (!host_offset)
        return ZZ_EINVAL;
    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i) {
        if (blocks[i].ptr == p) {
#ifndef ZZAPP_HOST_TEST
            *host_offset = blocks[i].card - ADDR_ADJ;
#else
            *host_offset = blocks[i].card;
#endif
            return ZZ_OK;
        }
    }
    return ZZ_EINVAL;
}

int zzapp_host_mem_info(zzapp_mem_info *out)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);

    if (!out)
        return ZZ_EINVAL;
    out->arena_size = host->mem ? ZZAPP_ARENA_BYTES : 0u;
    out->arena_used = host->heap_bump > host->heap_off ?
                      host->heap_bump - host->heap_off : 0u;
    out->arena_free = host->heap_end > host->heap_bump ?
                      host->heap_end - host->heap_bump : 0u;
    out->arena_largest_block = out->arena_free;
    out->host_visible_used = g_host_visible_used[slot];
    out->host_visible_limit = ZZAPP_HOST_VISIBLE_LIMIT;
    return ZZ_OK;
}
#ifdef ZZAPP_HOST_TEST
void zzapp_test_set_xtime(uint64_t ticks) { g_test_ticks = ticks; }
void zzapp_test_set_entry(void (*fn)(const void *api, int argc, char **argv)) { g_test_entry = fn; }
void zzapp_host_tick_at(uint64_t now) { g_test_ticks = now; service_tick_irq(now); }
void zzapp_test_set_time_hook(uint64_t (*hook)(void)) { g_test_time_hook = hook; }
void zzapp_test_request_reset(uint32_t slot)
{
    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return;
    request_slot_reset(slot);
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
uint8_t *zzapp_test_stack_canary(void)
{
    struct zzapp_host_slot *host = host_slot(current_slot());

    return (host->mem && host->stack_canary != 0u) ?
           host->mem + host->stack_canary : 0;
}
uint32_t zzapp_test_fault_code(void) { return coherent(current_slot())->fault; }
void zzapp_test_set_wait_registered(uint32_t slot, int registered)
{
    if (slot < ZZAPP_AS_SLOT_COUNT)
        g_test_wait_registered[slot] = registered ? 1u : 0u;
}
#endif
