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

#ifdef ZZAPP_HOST_TEST
static uint8_t g_test_wait_registered[ZZAPP_AS_SLOT_COUNT];
#endif

uint32_t __attribute__((weak)) zzapp_services_current_slot(void)
{
    return 0U;
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

void __attribute__((weak)) zzapp_services_set_terminal(uint32_t reason)
{
    (void)reason;
}

void __attribute__((weak)) zzapp_services_expire_claims(uint64_t now)
{
    (void)now;
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
    /* Bumped by every API entry; the 5 ms tick turns changes into the
     * progress and API-entry stamps, so a GL call reads no clock. */
    volatile uint32_t api_calls;
    /* A tick skipped scheduler work because an API call was running; the
     * call's epilogue catches up. */
    volatile uint32_t service_due;
    volatile uint64_t api_entered;
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
    int faulted;
    uint32_t seen_api_calls;
    uint64_t last_tick;
    volatile uint32_t watchdog_reason;
    uint32_t terminal_reason;
};

static struct zzapp_host_slot g_host[ZZAPP_AS_SLOT_COUNT];
static zzapp_proc_t g_proc[ZZAPP_AS_SLOT_COUNT];
static zzapp_kernel_t g_kernel;
static int g_cold_restarts;
/* Set in service_tick (possibly IRQ context); reported from run_app. */
#ifdef ZZAPP_HOST_TEST
static uint32_t g_test_service_calls;
void scheduler_core1_service_from_app(void) { g_test_service_calls++; }
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

/* The vectors restore from the returned frame and leave the exception-mode
 * SP just above it. Copy the chosen context onto the exception stack in
 * place, so that SP stays inside the exception stack and the next exception
 * cannot push into another thread's saved context. */
zzapp_arm_frame_t *zzapp_kernel_arch_dispatch(zzapp_arm_frame_t *frame,
                                               uint32_t source,
                                               uint32_t return_adjust)
{
    zzapp_arm_frame_t *next = zzapp_kernel_switch_frame(&g_kernel, frame,
        (zzapp_kernel_dispatch_t)source, return_adjust);

    if (next && next != frame)
        memcpy(frame, next, sizeof(*frame));
    return frame;
}

/* After an API call: a thread that is still runnable returns straight to
 * its caller without giving up its quantum. A call that blocked, slept or
 * stopped the thread hands the core to the scheduler. */
zzapp_arm_frame_t *zzapp_kernel_svc_return(zzapp_arm_frame_t *frame)
{
    int slot = zzapp_kernel_current_thread();

    if (slot >= 0 && slot < (int)ZZAPP_KERNEL_APP_SLOTS &&
        g_kernel.threads[slot].state == ZZAPP_THREAD_RUNNABLE)
        return frame;
    return zzapp_kernel_arch_dispatch(frame, ZZAPP_KERNEL_DISPATCH_VOLUNTARY, 0U);
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
    int slot = zzapp_kernel_current_thread();
    uint32_t selected;

    if (slot >= 0 && slot < (int)ZZAPP_AS_SLOT_COUNT)
        return (uint32_t)slot;
    selected = zzapp_services_current_slot();
    return selected < ZZAPP_AS_SLOT_COUNT ? selected : 0U;
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
        bytes > proc->heap_break - proc->heap_base)
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
    uint32_t bytes;
};
static struct zzapp_host_visible_block
    g_host_visible[ZZAPP_AS_SLOT_COUNT][ZZAPP_HOST_VISIBLE_SLOTS];
static volatile uint32_t g_host_visible_commit_active[ZZAPP_AS_SLOT_COUNT];
static uint32_t g_host_visible_used[ZZAPP_AS_SLOT_COUNT];

static void host_visible_release_all(void)
{
    struct zzapp_host_visible_block *blocks = g_host_visible[current_slot()];
    uint32_t i;

    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i) {
        if (!blocks[i].ptr)
            continue;
#ifndef ZZAPP_HOST_TEST
        (void)surface_allocator_free(blocks[i].card);
#else
        free(blocks[i].ptr);
#endif
        blocks[i].ptr = NULL;
    }
    g_host_visible_used[current_slot()] = 0;
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
#else
static int g_private_timer_ready;
static int g_firmware_text_readonly;
extern char __firmware_text_start[];
extern char __firmware_text_end[];
#endif

static uint64_t now_ticks(void)
{
#ifdef ZZAPP_HOST_TEST
    return g_test_ticks;
#else
    XTime now;
    XTime_GetTime(&now);
    return (uint64_t)now;
#endif
}

static uint64_t ticks_to_us(uint64_t ticks)
{
#ifdef ZZAPP_HOST_TEST
    return ticks;
#else
    return (ticks / (uint64_t)COUNTS_PER_SECOND) * 1000000ULL +
           ((ticks % (uint64_t)COUNTS_PER_SECOND) * 1000000ULL) /
           (uint64_t)COUNTS_PER_SECOND;
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

void zzapp_loader_bound(uint32_t card, uint8_t *mem, uint32_t bytes,
                        uint32_t owner, uint32_t generation)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);
    struct zzapp_coherent *coh = coherent(slot);

    host->card = card;
    host->mem = mem;
    host->owner = owner;
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

    if (requested_limit && requested_limit < limit)
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

static int stop_requested(void)
{
    return coherent(current_slot())->reset_request ||
           life_get32(ZZAPP_LIFE_STOP_OFF) != 0u || zzapp_svc_should_stop();
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


static void force_stop(uint32_t fault, uint32_t pc, uint32_t sp,
                       uint32_t lr, uint32_t far, uint32_t fsr, uint32_t cpsr)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);
    struct zzapp_coherent *coh = coherent(slot);

    (void)cpsr;
    if (!claim_fault(slot))
        return;
    coh->running = 0;
    coh->fault = fault;
    life32(ZZAPP_LIFE_FAULT_OFF, fault);
    life32(ZZAPP_LIFE_PC_OFF, pc);
    life32(ZZAPP_LIFE_SP_OFF, sp);
    life32(ZZAPP_LIFE_LR_OFF, lr);
    life32(ZZAPP_LIFE_FAR_OFF, far);
    life32(ZZAPP_LIFE_FSR_OFF, fsr);
    zzapp_kernel_stop_app(&g_kernel, slot);
    if (zzapp_kernel_current_thread() >= 0)
        zzapp_kernel_service_due(&g_kernel);
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

    coh->in_api = 0u;
    coh->service_due = 0u;
    host_slot(slot)->seen_api_calls = coh->api_calls;
    host_slot(slot)->last_tick = now_ticks();
}
static void check_stack_canary(void);

static void service_tick(uint64_t now, int permit_reentry)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);
    struct zzapp_coherent *coh = coherent(slot);
    uint32_t calls = coh->api_calls;

    coh->tick_seen = now;
    check_stack_canary();
    if (calls != host->seen_api_calls) {
        host->seen_api_calls = calls;
        coh->progress = now;
        life_progress(now);
        if (coh->in_api != 0u && !permit_reentry)
            coh->api_entered = host->last_tick;
    }
    host->last_tick = now;
    if (coh->in_api == 0u || permit_reentry) {
        zzapp_services_poll();
        coh->service_due = 0u;
#ifdef ZZAPP_HOST_TEST
        scheduler_core1_service_from_app();
#else
        zzapp_kernel_service_due(&g_kernel);
#endif
    } else {
        coh->service_due = 1u;
    }
    if (!g_host_visible_commit_active[slot] && coh->reset_request) {
        force_stop(ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
    } else if (!g_host_visible_commit_active[slot] && stop_requested()) {
        if (!host->stop_mark)
            host->stop_mark = now;
        if (ticks_to_us(now - host->stop_mark) >= ZZAPP_STOP_GRACE_US)
            force_stop(ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
    } else {
        host->stop_mark = 0;
    }
    if (!g_host_visible_commit_active[slot] && coh->running &&
        coh->in_api == 0u &&
        ticks_to_us(now - coh->progress) >= ZZAPP_APP_STALL_US) {
        host->watchdog_reason = 1u;
        force_stop(ZZAPP_FAULT_WATCHDOG, 0, 0, 0, 0, 0, 0);
    }
    if (!permit_reentry && !g_host_visible_commit_active[slot] && coh->running &&
        coh->in_api != 0u &&
        ticks_to_us(now - coh->api_entered) >= ZZAPP_API_STUCK_US) {
        host->watchdog_reason = 2u;
        force_stop(ZZAPP_FAULT_WATCHDOG, 0, 0, 0, 0, 0, 0);
    }
    maybe_expire_fault(now);
}


#ifndef ZZAPP_HOST_TEST
static void zzapp_private_timer_irq(void *unused)
{
    (void)unused;
    Xil_Out32(XPAR_PS7_SCUTIMER_0_BASEADDR + XSCUTIMER_ISR_OFFSET,
              XSCUTIMER_ISR_EVENT_FLAG_MASK);
    if (coherent(current_slot())->running)
        service_tick(now_ticks(), 0);
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

#define service_tick_in_task service_tick

/* The 64-byte canary below the app stack is set in run_app. Docs promise a
 * force-stop with ZZAPP_FAULT_DATA when it is damaged; check it on every
 * service pass. */
static void check_stack_canary(void)
{
    struct zzapp_host_slot *host = host_slot(current_slot());
    uint32_t i;

    if (!host->mem || host->heap_end == 0u || !coherent(current_slot())->running)
        return;
#ifdef ZZAPP_HOST_TEST
    for (i = 0; i < ZZAPP_CANARY_BYTES; i++) {
        if (host->mem[host->heap_end + i] != 0xA5u) {
            force_stop(ZZAPP_FAULT_DATA, 0, 0, 0, 0, 0, 0);
            return;
        }
    }
#else
    {
        uint8_t *canary = proc_alias(host->heap_end, ZZAPP_CANARY_BYTES);

        if (!canary)
            return;
        for (i = 0; i < ZZAPP_CANARY_BYTES; i++)
            if (canary[i] != 0xA5u) {
                force_stop(ZZAPP_FAULT_DATA, 0, 0, 0, 0, 0, 0);
                return;
            }
    }
#endif
}

void zzapp_host_yield(void)
{
    zzapp_note_progress();
    check_stack_canary();
    service_tick_in_task(now_ticks(), 0);
}

void zzapp_host_yield_wait(void)
{
    uint32_t slot = current_slot();
    struct zzapp_coherent *coh = coherent(slot);

    if (zzapp_kernel_current_thread() < 0) {
        zzapp_note_progress();
        check_stack_canary();
        service_tick_in_task(now_ticks(), 1);
        return;
    }
    if (coh->in_api == 0u) {
        zzapp_host_yield();
        return;
    }
    (void)zzapp_kernel_api_block(&g_kernel, slot);
    if (!zzapp_services_wait_registered(slot))
        (void)zzapp_kernel_sleep(&g_kernel, slot, g_kernel.now + 1U);
    zzapp_note_progress();
#ifndef ZZAPP_HOST_TEST
    /*
     * The core-1 SVC vector recognises a privileged SVC as a voluntary
     * scheduler handoff.  It resumes here only after the waiter is woken.
     */
    __asm__ __volatile__("svc 0" ::: "memory");
#endif
    (void)zzapp_kernel_api_lock(&g_kernel, slot);
}
void zzapp_host_sleep(uint32_t us)
{
    uint32_t slot = current_slot();

    if (zzapp_kernel_current_thread() < 0)
        return;
    (void)zzapp_kernel_sleep(&g_kernel, slot,
        g_kernel.now + (uint64_t)((us + 999u) / 1000u));
#ifndef ZZAPP_HOST_TEST
    __asm__ __volatile__("svc 0" ::: "memory");
#endif
    (void)zzapp_kernel_api_lock(&g_kernel, slot);
}


uint64_t zzapp_host_time_us(void) { return ticks_to_us(now_ticks()); }

/*
 * Every GL call of an ARM app passes through here (GLQuake makes tens of
 * thousands per frame), so neither side reads the clock or touches the
 * uncached lifecycle block; service_tick derives the stamps.
 */
void zzapp_api_enter(void)
{
    uint32_t slot = current_slot();
    struct zzapp_coherent *coh = coherent(slot);

    if (zzapp_kernel_current_thread() >= 0)
        (void)zzapp_kernel_api_lock(&g_kernel, slot);
    coh->api_calls++;
    coh->in_api++;
}

/* Hot non-blocking calls (should_stop, time_us). Count as an API call so a
 * loop that only polls them is not a 30 s stall, without the in-API depth
 * or the leave-path service tick. */
void zzapp_api_touch(void)
{
    coherent(current_slot())->api_calls++;
}

void zzapp_api_leave(void)
{
    uint32_t slot = current_slot();
    struct zzapp_coherent *coh = coherent(slot);

    if (coh->in_api)
        coh->in_api--;
    if (zzapp_kernel_current_thread() >= 0)
        (void)zzapp_kernel_api_unlock(&g_kernel, slot);
    if (!coh->in_api && coh->service_due)
        service_tick(now_ticks(), 0);
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

static int copy_argv(uint32_t argc, char ***argv_out)
{
    struct zzapp_host_slot *host = host_slot(current_slot());
    uint8_t *window = (uint8_t *)zzapp_services_window();
    uint32_t i;
    uint32_t total = (argc + 1u) * sizeof(uint32_t);
    uint32_t argv_offset;
    uint8_t *argv;

    if (argc > ZZAPP_ARGV_MAX || (argc && !window))
        return argc ? ZZ_EIO : ZZ_EINVAL;
    for (i = 0; i < argc; i++) {
        uint32_t off = zzapp_get_be32(window + ZZAPP_ARGV_OFF +
                                      ZZAPP_ARGV_OFFS_OFF + 4u * i);
        uint32_t n = 0;

        if (off < ZZAPP_ARGV_OFF || off >= ZZAPP_ARGV_OFF + ZZAPP_ARGV_BYTES)
            return ZZ_EINVAL;
        while (off + n < ZZAPP_ARGV_OFF + ZZAPP_ARGV_BYTES && window[off + n])
            n++;
        if (off + n >= ZZAPP_ARGV_OFF + ZZAPP_ARGV_BYTES || n >= ZZAPP_ARGV_STR_MAX)
            return ZZ_EINVAL;
        total += n + 1u;
    }
    total = (total + 7u) & ~7u;
    if (host->heap_bump + total > host->heap_end)
        return ZZ_ENOMEM;
    argv_offset = host->heap_bump;
    argv = proc_alias(argv_offset, (argc + 1u) * sizeof(uint32_t));
    if (!argv)
        return ZZ_ENOMEM;
    host->heap_bump += (argc + 1u) * sizeof(uint32_t);
    for (i = 0; i < argc; i++) {
        uint32_t off = zzapp_get_be32(window + ZZAPP_ARGV_OFF +
                                      ZZAPP_ARGV_OFFS_OFF + 4u * i);
        uint32_t n = 0;
        uint32_t value = ZZAPP_AS_WINDOW_BASE + host->heap_bump;
        uint8_t *dst;

        while (window[off + n])
            n++;
        dst = proc_alias(host->heap_bump, n + 1u);
        if (!dst)
            return ZZ_ENOMEM;
        memcpy(dst, window + off, n);
        dst[n] = '\0';
        memcpy(argv + i * sizeof(value), &value, sizeof(value));
        host->heap_bump += n + 1u;
    }
    memset(argv + argc * sizeof(uint32_t), 0, sizeof(uint32_t));
    *argv_out = (char **)(uintptr_t)(ZZAPP_AS_WINDOW_BASE + argv_offset);
    return ZZ_OK;
}
#else
static int copy_argv(uint32_t argc, char ***argv_out)
{
    struct zzapp_host_slot *host = host_slot(current_slot());
    uint8_t *window = (uint8_t *)zzapp_services_window();
    char **argv;
    uint32_t i;
    uint32_t total = (argc + 1u) * (uint32_t)sizeof(*argv);

    if (argc > ZZAPP_ARGV_MAX || !host->mem)
        return ZZ_EINVAL;
    if (argc && !window)
        return ZZ_EIO;
    for (i = 0; i < argc; i++) {
        uint32_t off = zzapp_get_be32(window + ZZAPP_ARGV_OFF + ZZAPP_ARGV_OFFS_OFF + 4u * i);
        uint32_t n = 0;
        if (off < ZZAPP_ARGV_OFF || off >= ZZAPP_ARGV_OFF + ZZAPP_ARGV_BYTES)
            return ZZ_EINVAL;
        while (off + n < ZZAPP_ARGV_OFF + ZZAPP_ARGV_BYTES && window[off + n])
            n++;
        if (off + n >= ZZAPP_ARGV_OFF + ZZAPP_ARGV_BYTES || n >= ZZAPP_ARGV_STR_MAX)
            return ZZ_EINVAL;
        total += n + 1u;
    }
    total = (total + 7u) & ~7u;
    if (host->heap_bump + total > host->heap_end)
        return ZZ_ENOMEM;
    argv = (char **)(host->mem + host->heap_bump);
    host->heap_bump += (argc + 1u) * (uint32_t)sizeof(*argv);
    for (i = 0; i < argc; i++) {
        uint32_t off = zzapp_get_be32(window + ZZAPP_ARGV_OFF + ZZAPP_ARGV_OFFS_OFF + 4u * i);
        char *dst = (char *)(host->mem + host->heap_bump);
        uint32_t n = 0;
        while (window[off + n]) { dst[n] = (char)window[off + n]; n++; }
        dst[n++] = '\0';
        argv[i] = dst;
        host->heap_bump += n;
    }
    argv[argc] = NULL;
    *argv_out = argv;
    return ZZ_OK;
}
#endif

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

static void start_failed(int status)
{
    zzapp_services_start_entered(status);
    zzapp_loader_release();
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

    if (!host->image_loaded || !host->mem) { start_failed(ZZ_EINVAL); return; }
    host->heap_off = (host->image_end + 7u) & ~7u;
#ifdef ZZAPP_HOST_TEST
    host->heap_end = ZZAPP_ARENA_BYTES - host->stack_bytes;
#else
    host->heap_end = proc->heap_limit;
#endif
    host->heap_bump = host->heap_off;
    if (host->heap_end <= host->heap_off) { start_failed(ZZ_ENOMEM); return; }
    rc = copy_argv(argc, &argv);
    if (rc != ZZ_OK) { start_failed(rc); return; }
    proc->heap_base = host->heap_bump;
    proc->heap_break = host->heap_bump;
    proc->heap_limit = host->heap_end;
#ifndef ZZAPP_HOST_TEST
    if (!protect_firmware_text()) {
        start_failed(ZZ_EIO);
        return;
    }
    app_api = zzapp_svc_api_for_slot(slot, zzapp_api_table());
    if (!app_api ||
        zzapp_as_grant_user_ro(&proc->as,
            (uint32_t)zzapp_svc_api_veneer(slot) & ~0xfffU, 4096U, 1) ||
        zzapp_as_grant_user_ro(&proc->as,
            (uint32_t)(uintptr_t)app_api & ~0xfffU, 4096U, 0) ||
        zzapp_as_grant_user_ro(&proc->as,
            (uint32_t)(uintptr_t)zzapp_svc_gl_for_slot(slot) & ~0xfffU,
            4096U, 0)) {
        start_failed(ZZ_EIO);
        return;
    }
#endif
#ifdef ZZAPP_HOST_TEST
    app_api = zzapp_api_table();
    memset(host->mem + host->heap_end, 0xA5, ZZAPP_CANARY_BYTES);
#else
    {
        uint8_t *canary = proc_alias(host->heap_end, ZZAPP_CANARY_BYTES);

        if (!canary) { start_failed(ZZ_ENOMEM); return; }
        memset(canary, 0xA5, ZZAPP_CANARY_BYTES);
    }
#endif
    host->stop_mark = 0;
    host->fault_mark = 0;
    host->faulted = 0;
    host->exit_code = 0;
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
        start_failed(ZZ_EINVAL);
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
    coh->running = 0;
    host->watchdog_reason = 0u;
    if (!host->faulted) {
        if (coh->reset_request) {
            zzapp_services_set_terminal(ZZAPP_END_AMIGA_RESET);
            zzapp_services_on_reset();
        } else {
            life32(ZZAPP_LIFE_EXIT_OFF, host->exit_code);
            zzapp_services_set_terminal(ZZAPP_END_EXIT);
            zzapp_services_on_app_end();
        }
        zzapp_minigl_reclaim(coh->generation);
        publish_state(ZZAPP_ST_IDLE);
        zzapp_loader_release();
    }
#else
    zzapp_kernel_set_current_thread(-1);
    zzapp_private_timer_enable();
#endif
}

void zzapp_worker_hook(void)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);
    struct zzapp_coherent *coh = coherent(slot);
    uint32_t argc = 0;

    if (coh->reset_request) {
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
        host->stop_mark = 0;
        host->fault_mark = 0;
        coherent_barrier();
        coh->reset_ack = 1;
        coh->reset_request = 0;
        coherent_publish();
        return;
    }
    zzapp_services_poll();
    if (host->state == ZZAPP_ST_LOADING && stop_requested()) {
        zzapp_services_cancel_start(ZZ_EIO);
        (void)zzapp_loader_abort();
        return;
    }
    if (host->state == ZZAPP_ST_LOADING && zzapp_services_take_start(&argc)) {
        publish_state(ZZAPP_ST_STARTING);
        run_app(argc);
    }
}

int zzapp_fault_is_app(uint32_t pc, uint32_t abort_lr)
{
    uint32_t slot = current_slot();

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

int zzapp_fault_wants_restart(uint32_t pc, uint32_t abort_lr)
{
    return zzapp_fault_is_app(pc, abort_lr) ? 0 : 1;
}

void zzapp_take_app_fault(uint32_t fault, uint32_t pc, uint32_t sp,
                          uint32_t lr, uint32_t far, uint32_t fsr,
                          uint32_t cpsr)
{
    force_stop(fault, pc, sp, lr, far, fsr, cpsr);
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
    zzapp_services_expire_claims(ticks_to_us(now_ticks()));

    for (slot = 0U; slot < ZZAPP_AS_SLOT_COUNT; ++slot) {
        struct zzapp_host_slot *host = host_slot(slot);
        struct zzapp_coherent *coh = coherent(slot);
        uint32_t running = coh->running;
        uint64_t age;
        uint64_t tick_age;
        uint64_t now;
        uint32_t heartbeat;

        zzapp_services_select_slot(slot);
        coherent_barrier();
        age = core1_stamp_age_us(&coh->progress);
        tick_age = core1_stamp_age_us(&coh->tick_seen);
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
            life32(ZZAPP_LIFE_STOP_OFF, 1u);
        }
        if (running && age >= ZZAPP_WATCHDOG_US &&
            tick_age >= ZZAPP_WATCHDOG_US) {
            coh->running = 0;
            coh->fault = ZZAPP_FAULT_WATCHDOG_RESTART;
            life32(ZZAPP_LIFE_FAULT_OFF, ZZAPP_FAULT_WATCHDOG_RESTART);
            (void)claim_fault(slot);
            host->fault_mark = now_ticks();
            publish_state(ZZAPP_ST_FAULTED);
            zzapp_services_set_terminal(ZZAPP_END_CORE_RESTART);
#ifndef ZZAPP_HOST_TEST
            printf("[app] watchdog restart: no progress for %lu ms, no tick for "
                   "%lu ms, in_api=%lu\r\n", (unsigned long)(age / 1000u),
                   (unsigned long)(tick_age / 1000u),
                   (unsigned long)coh->in_api);
            core1_cold_restart();
#endif
            zzapp_services_on_app_end();
            zzapp_minigl_reclaim(coh->generation);
            if (zzapp_api_poison_resources)
                zzapp_api_poison_resources();
            zzapp_loader_release();
        }
    }
    zzapp_services_select_slot(selected);
}

void zzapp_on_core1_restart(void)
{
    uint32_t slot;
    uint32_t selected = zzapp_services_current_slot();

    for (slot = 0U; slot < ZZAPP_AS_SLOT_COUNT; ++slot) {
        zzapp_services_select_slot(slot);
        if (coherent(slot)->running) {
            host_slot(slot)->terminal_reason = ZZAPP_END_CORE_RESTART;
            force_stop(ZZAPP_FAULT_WATCHDOG_RESTART, 0, 0, 0, 0, 0, 0);
        } else if (zzapp_services_state() != ZZAPP_ST_IDLE) {
            zzapp_services_set_terminal(ZZAPP_END_CORE_RESTART);
            zzapp_services_on_reset();
        }
    }
    zzapp_services_select_slot(selected);
    g_cold_restarts++;
}

/* A running app stops itself within one private-timer tick and the worker
 * acks after its teardown.  Bounded well under the 2 s API-stall limit. */
#define ZZAPP_RESET_ACK_WAIT_US 1000000u

void zzapp_on_amiga_reset(void)
{
    uint32_t slot;
    uint32_t selected = zzapp_services_current_slot();

    coherent_barrier();
    for (slot = 0U; slot < ZZAPP_AS_SLOT_COUNT; ++slot) {
        coherent(slot)->reset_ack = 0;
        coherent(slot)->reset_request = 1;
        host_slot(slot)->terminal_reason = ZZAPP_END_AMIGA_RESET;
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
    zzapp_kernel_init(&g_kernel);
    zzapp_loader_memory_init();
    g_cold_restarts = 0;
#ifdef ZZAPP_HOST_TEST
    g_test_service_calls = 0;
    memset(g_test_wait_registered, 0, sizeof(g_test_wait_registered));
#endif
}

void zzapp_host_exit(int code)
{
    uint32_t slot = current_slot();
    struct zzapp_host_slot *host = host_slot(slot);

    host->exit_code = (uint32_t)code;
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
            zzapp_services_set_terminal(ZZAPP_END_EXIT);
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
        !bytes || align < 8u || (align & (align - 1u)) != 0u)
        return NULL;
    if ((flags & ZZ_ALLOC_HOST_VISIBLE) == 0u ||
        bytes > ZZAPP_HOST_VISIBLE_LIMIT - g_host_visible_used[slot])
        return NULL;
    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i) {
        uint32_t card;
        void *ptr;

        if (blocks[i].ptr)
            continue;
        zzapp_host_commit_begin();
#ifndef ZZAPP_HOST_TEST
        card = surface_allocator_alloc_aligned(bytes, align);
        ptr = (void *)(uintptr_t)card;
#else
        ptr = malloc(bytes);
        card = 0x033f0000u + g_host_visible_used[slot];
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
    struct zzapp_host_slot *host = host_slot(current_slot());

    if (base)
        *base = host->mem ? host->mem + host->heap_off : NULL;
    if (bytes)
        *bytes = host->heap_end > host->heap_off ?
                 host->heap_end - host->heap_off : 0;
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
void zzapp_host_tick_at(uint64_t now) { g_test_ticks = now; service_tick(now, 0); }
void zzapp_test_mark_running(void)
{
    uint32_t slot = current_slot();

    coherent(slot)->running = 1;
    host_slot(slot)->state = ZZAPP_ST_RUNNING;
    zzapp_note_progress();
    begin_tick_observation();
}
uint32_t zzapp_test_service_calls(void) { return g_test_service_calls; }
uint32_t zzapp_test_in_api(void) { return coherent(current_slot())->in_api; }
int zzapp_test_cold_restarts(void) { return g_cold_restarts; }
uint32_t zzapp_test_exit_code(void) { return host_slot(current_slot())->exit_code; }
uint8_t *zzapp_test_stack_canary(void)
{
    struct zzapp_host_slot *host = host_slot(current_slot());

    return (host->mem && host->heap_end != 0u) ? host->mem + host->heap_end : 0;
}
uint32_t zzapp_test_fault_code(void) { return coherent(current_slot())->fault; }
void zzapp_test_set_wait_registered(uint32_t slot, int registered)
{
    if (slot < ZZAPP_AS_SLOT_COUNT)
        g_test_wait_registered[slot] = registered ? 1u : 0u;
}
#endif
