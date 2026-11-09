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
#ifdef ZZAPP_HOST_TEST
static uint32_t g_test_service_calls;
void scheduler_core1_service_from_app(void) { g_test_service_calls++; }
#endif

#ifndef ZZAPP_HOST_TEST
static struct zzapp_coherent *const g_coh =
    (struct zzapp_coherent *)(SDK_TASKQ_REGION_ADDRESS + ZZAPP_COHERENT_OFFSET);
#else
static struct zzapp_coherent g_coh_store;
static struct zzapp_coherent *const g_coh = &g_coh_store;
static uint64_t g_test_ticks;
#endif

static uint8_t *g_mem;
static uint32_t g_card;
static uint32_t g_owner;
static uint32_t g_state;
static uint32_t g_entry;
static int g_image_loaded;
static uint32_t g_image_end;
static uint32_t g_stack_bytes;
static uint64_t g_stop_mark;
static uint64_t g_fault_mark;
static uint32_t g_exit_code;
static uint32_t g_heap_off;
static uint32_t g_heap_end;
static uint32_t g_heap_bump;
static int g_faulted;
static int g_cold_restarts;
/* Set in service_tick (possibly IRQ context); reported from run_app. */
static volatile uint32_t g_watchdog_reason;
static zzapp_proc_t g_proc[ZZAPP_AS_SLOT_COUNT];
static zzapp_kernel_t g_kernel;

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

    return slot >= 0 && slot < (int)ZZAPP_AS_SLOT_COUNT ?
           (uint32_t)slot : 0u;
}

int zzapp_proc_heap_grow(uint32_t bytes, void **old_break)
{
    zzapp_proc_t *proc = &g_proc[0];

    if (!bytes || bytes % ZZAPP_MEM_FRAME_BYTES ||
        proc->note_minor < 3u ||
        bytes > proc->heap_limit - proc->heap_break ||
        bytes > proc->memory.limit - proc->memory.committed)
        return ZZ_ENOMEM;
    if (old_break)
#ifdef ZZAPP_HOST_TEST
        *old_break = g_mem + proc->heap_break;
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
    zzapp_proc_t *proc = &g_proc[0];

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
        *new_break = g_mem + proc->heap_break;
#else
        *new_break = (void *)(uintptr_t)(ZZAPP_AS_WINDOW_BASE + proc->heap_break);
#endif
    return ZZ_OK;
}

int zzapp_proc_heap_info(uint32_t *committed, uint32_t *limit,
                         uint32_t *pool_free)
{
    zzapp_proc_t *proc = &g_proc[0];

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
static struct zzapp_host_visible_block g_host_visible[ZZAPP_HOST_VISIBLE_SLOTS];
static volatile uint32_t g_host_visible_commit_active;
static uint32_t g_host_visible_used;

static void host_visible_release_all(void)
{
    uint32_t i;

    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i) {
        if (!g_host_visible[i].ptr)
            continue;
#ifndef ZZAPP_HOST_TEST
        (void)surface_allocator_free(g_host_visible[i].card);
#else
        free(g_host_visible[i].ptr);
#endif
        g_host_visible[i].ptr = NULL;
    }
    g_host_visible_used = 0;
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

/* g_state is the runtime authority; this is its sole lifecycle-state mirror. */
static void publish_state(uint32_t state)
{
    g_state = state;
    coherent_publish();
    life32(ZZAPP_LIFE_STATE_OFF, state);
}

void zzapp_loader_bound(uint32_t card, uint8_t *mem, uint32_t bytes,
                        uint32_t owner, uint32_t generation)
{
    g_card = card;
    g_mem = mem;
    g_owner = owner;
    g_proc[0].owner = owner;
    g_proc[0].generation = generation;
    g_coh->running = 0;
    g_coh->in_api = 0;
    g_coh->fault = ZZAPP_FAULT_NONE;
    g_coh->arena_card = card;
    g_coh->arena_bytes = bytes;
    g_coh->owner = owner;
    g_coh->generation = generation;
    life32(ZZAPP_LIFE_MAGIC_OFF, ZZAPP_LIFE_MAGIC);
    life32(ZZAPP_LIFE_VERSION_OFF, ZZAPP_LIFE_VERSION);
    life32(ZZAPP_LIFE_GENERATION_OFF, generation);
    /* The window outlives sessions; a fault without registers (watchdog
     * restart) must not report the previous app's pc/far/fsr. */
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

    host_visible_release_all();
    if (zzapp_api_reclaim_resources)
        zzapp_api_reclaim_resources();
    zzapp_kernel_stop_app(&g_kernel, slot);
    g_card = 0;
    g_mem = NULL;
    g_owner = 0;
    g_entry = 0;
    g_image_loaded = 0;
    g_image_end = 0;
    g_stack_bytes = 0;
    memset(&g_proc[slot], 0, sizeof(g_proc[slot]));
    g_coh->arena_card = 0;
    g_coh->arena_bytes = 0;
    g_coh->owner = 0;
}

uint32_t zzapp_loader_generation(void) { return g_coh->generation; }
uint32_t zzapp_loader_state(void) { return g_state; }
void zzapp_loader_set_state(uint32_t state) { publish_state(state); }
uint32_t zzapp_loader_reset_requested(void) { return g_coh->reset_request; }
int zzapp_loader_have_image(void) { return g_image_loaded; }

int zzapp_loader_set_image(uint32_t entry, uint32_t image_end,
                           uint32_t stack_bytes, uint32_t note_flags,
                           uint32_t note_minor, uint32_t min_heap_bytes)
{
    zzapp_proc_t *proc = &g_proc[0];
    uint32_t image_bytes = (image_end + ZZAPP_MEM_FRAME_BYTES - 1u) &
                           ~(ZZAPP_MEM_FRAME_BYTES - 1u);
    uint32_t stack_commit = (stack_bytes + ZZAPP_MEM_FRAME_BYTES - 1u) &
                            ~(ZZAPP_MEM_FRAME_BYTES - 1u);
    uint32_t commit = note_minor < 3u ? ZZAPP_ARENA_BYTES :
                      image_bytes + stack_commit + min_heap_bytes;

    if (commit > ZZAPP_MEM_PRIVATE_WINDOW_BYTES)
        return ZZ_ENOMEM;
    g_entry = entry;
    g_image_loaded = 1;
    g_image_end = image_end;
    g_stack_bytes = stack_bytes;
    proc->image_entry = entry;
    proc->image_end = image_end;
    proc->image_frame_count = image_bytes / ZZAPP_MEM_FRAME_BYTES;
    proc->stack_frame_count = stack_commit / ZZAPP_MEM_FRAME_BYTES;
    proc->stack_bytes = stack_bytes;
    proc->note_minor = note_minor;
    proc->memory.committed = commit;
    proc->memory.limit = note_minor < 3u ? ZZAPP_ARENA_BYTES :
                         ZZAPP_MEM_PRIVATE_WINDOW_BYTES;
    life32(ZZAPP_LIFE_FLAGS_OFF,
           note_flags & (ZZAPP_F_DISPLAY | ZZAPP_F_AUDIO | ZZAPP_F_GL));
    return ZZ_OK;
}

uint32_t zzapp_current_generation(void) { return g_coh->generation; }
uint32_t zzapp_runtime_state(void) { return g_state; }
uint32_t zzapp_runtime_caps(void) { return ZZAPP_CAP_Z3 | ZZAPP_CAP_DISPLAY | ZZAPP_CAP_AUDIO | ZZAPP_CAP_GL | ZZAPP_CAP_CLUT8; }
int zzapp_host_faulted(void) { return g_faulted; }

void zzapp_note_progress(void)
{
    uint64_t now = now_ticks();
    g_coh->progress = now;
    life_progress(now);
}

static int stop_requested(void)
{
    return g_coh->reset_request || life_get32(ZZAPP_LIFE_STOP_OFF) != 0u ||
           zzapp_svc_should_stop();
}

/* Core 1 (service tick, faults) and core 0 (restart recovery) can both stop
 * the app; exactly one of them runs the teardown. */
static int claim_fault(void)
{
    return __atomic_exchange_n(&g_faulted, 1, __ATOMIC_ACQ_REL) == 0;
}

static void force_stop(uint32_t fault, uint32_t pc, uint32_t sp,
                       uint32_t lr, uint32_t far, uint32_t fsr, uint32_t cpsr)
{
    (void)cpsr;
    if (!claim_fault())
        return;
    zzapp_private_timer_disable();
    g_coh->running = 0;
    g_coh->fault = fault;
    life32(ZZAPP_LIFE_FAULT_OFF, fault);
    life32(ZZAPP_LIFE_PC_OFF, pc);
    life32(ZZAPP_LIFE_SP_OFF, sp);
    life32(ZZAPP_LIFE_LR_OFF, lr);
    life32(ZZAPP_LIFE_FAR_OFF, far);
    life32(ZZAPP_LIFE_FSR_OFF, fsr);
    if (zzapp_kernel_current_thread() >= 0) {
        uint32_t slot = current_slot();

        zzapp_kernel_stop_app(&g_kernel, slot);
        zzapp_kernel_service_due(&g_kernel);
    }
#ifndef ZZAPP_HOST_TEST
    (void)cpsr;
    {
        /* The caller chain, one line: pc/lr alone have twice pointed at a
         * frame whose inputs came from further up. */
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
    g_fault_mark = now_ticks();
    zzapp_services_on_app_end();
    zzapp_minigl_reclaim(g_coh->generation);
#ifndef ZZAPP_HOST_TEST
    if (g_owner)
        card_pool_release_owner(&card_pool, g_owner);
#endif
#ifdef ZZAPP_HOST_TEST
    /* The host harness cannot discard the C app frame. Mirror the worker's
     * post-unwind reset hand-off so a reset START has no stale completion. */
    if (g_coh->reset_request)
        zzapp_services_on_reset();
#endif
    if (zzapp_api_poison_resources)
        zzapp_api_poison_resources();
    zzapp_loader_unbound();
}

static void maybe_expire_fault(uint64_t now)
{
    /* Core 1 may set the mark after the caller sampled now. */
    if (g_state == ZZAPP_ST_FAULTED && now > g_fault_mark &&
        ticks_to_us(now - g_fault_mark) >= ZZAPP_WATCHDOG_US)
        publish_state(ZZAPP_ST_IDLE);
}

/* Core 1 only: the API call count and time the previous tick observed. */
static uint32_t g_seen_api_calls;
static uint64_t g_last_tick;

static void begin_tick_observation(void)
{
    g_coh->in_api = 0u;
    g_coh->service_due = 0u;
    g_seen_api_calls = g_coh->api_calls;
    g_last_tick = now_ticks();
}
static void check_stack_canary(void);

static void service_tick(uint64_t now, int permit_reentry)
{
    uint32_t calls = g_coh->api_calls;

    g_coh->tick_seen = now;
    check_stack_canary();
    if (calls != g_seen_api_calls) {
        /* The app made API calls since the previous tick.  One that is
         * still running started no earlier than that tick.  Declared
         * waits (permit_reentry) set their own entry stamp. */
        g_seen_api_calls = calls;
        g_coh->progress = now;
        life_progress(now);
        if (g_coh->in_api != 0u && !permit_reentry)
            g_coh->api_entered = g_last_tick;
    }
    g_last_tick = now;
    /*
     * Privileged work never enters an API body. A declared wait is the only
     * yield point that permits it before the matching API leave.
     */
    if (g_coh->in_api == 0u || permit_reentry) {
        zzapp_services_poll();
        g_coh->service_due = 0u;
#ifdef ZZAPP_HOST_TEST
        scheduler_core1_service_from_app();
#else
        zzapp_kernel_service_due(&g_kernel);
#endif
    } else {
        g_coh->service_due = 1u;
    }
    if (!g_host_visible_commit_active && g_coh->reset_request) {
        force_stop(ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
    } else if (!g_host_visible_commit_active && stop_requested()) {
        if (!g_stop_mark)
            g_stop_mark = now;
        if (ticks_to_us(now - g_stop_mark) >= ZZAPP_STOP_GRACE_US)
            force_stop(ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
    } else {
        g_stop_mark = 0;
    }
    if (!g_host_visible_commit_active && g_coh->running &&
        g_coh->in_api == 0u &&
        ticks_to_us(now - g_coh->progress) >= ZZAPP_APP_STALL_US) {
        g_watchdog_reason = 1u;
        force_stop(ZZAPP_FAULT_WATCHDOG, 0, 0, 0, 0, 0, 0);
    }
    if (!g_host_visible_commit_active && g_coh->running &&
        g_coh->in_api != 0u &&
        ticks_to_us(now - g_coh->api_entered) >= ZZAPP_API_STUCK_US) {
        g_watchdog_reason = 2u;
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
    if (g_coh->running)
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
    uint32_t i;

    if (!g_mem || g_heap_end == 0u || !g_coh->running)
        return;
#ifdef ZZAPP_HOST_TEST
    for (i = 0; i < ZZAPP_CANARY_BYTES; i++) {
        if (g_mem[g_heap_end + i] != 0xA5u) {
            force_stop(ZZAPP_FAULT_DATA, 0, 0, 0, 0, 0, 0);
            return;
        }
    }
#else
    {
        uint8_t *canary = proc_alias(g_heap_end, ZZAPP_CANARY_BYTES);

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
    uint64_t now = now_ticks();
    uint32_t slot = current_slot();
    int blocked = zzapp_kernel_current_thread() >= 0 && g_coh->in_api != 0u;

    if (g_coh->in_api != 0u)
        g_coh->api_entered = now;
    if (blocked)
        (void)zzapp_kernel_api_block(&g_kernel, slot);
    zzapp_note_progress();
    service_tick_in_task(now, 1);
    if (blocked) {
        (void)zzapp_kernel_wake(&g_kernel, slot, ZZAPP_WAKE_READY);
        (void)zzapp_kernel_api_lock(&g_kernel, slot);
    }
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

    if (zzapp_kernel_current_thread() >= 0)
        (void)zzapp_kernel_api_lock(&g_kernel, slot);
    g_coh->api_calls++;
    g_coh->in_api++;
}

/* Hot non-blocking calls (should_stop, time_us). Count as an API call so a
 * loop that only polls them is not a 30 s stall, without the in-API depth
 * or the leave-path service tick. */
void zzapp_api_touch(void)
{
    g_coh->api_calls++;
}

void zzapp_api_leave(void)
{
    uint32_t slot = current_slot();

    if (g_coh->in_api)
        g_coh->in_api--;
    if (zzapp_kernel_current_thread() >= 0)
        (void)zzapp_kernel_api_unlock(&g_kernel, slot);
    if (!g_coh->in_api && g_coh->service_due)
        service_tick(now_ticks(), 0);
}

#ifndef ZZAPP_HOST_TEST
static uint8_t *proc_alias(uint32_t offset, uint32_t bytes)
{
    zzapp_proc_t *proc = &g_proc[0];
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
    if (g_heap_bump + total > g_heap_end)
        return ZZ_ENOMEM;
    argv_offset = g_heap_bump;
    argv = proc_alias(argv_offset, (argc + 1u) * sizeof(uint32_t));
    if (!argv)
        return ZZ_ENOMEM;
    g_heap_bump += (argc + 1u) * sizeof(uint32_t);
    for (i = 0; i < argc; i++) {
        uint32_t off = zzapp_get_be32(window + ZZAPP_ARGV_OFF +
                                      ZZAPP_ARGV_OFFS_OFF + 4u * i);
        uint32_t n = 0;
        uint32_t value = ZZAPP_AS_WINDOW_BASE + g_heap_bump;
        uint8_t *dst;

        while (window[off + n])
            n++;
        dst = proc_alias(g_heap_bump, n + 1u);
        if (!dst)
            return ZZ_ENOMEM;
        memcpy(dst, window + off, n);
        dst[n] = '\0';
        memcpy(argv + i * sizeof(value), &value, sizeof(value));
        g_heap_bump += n + 1u;
    }
    memset(argv + argc * sizeof(uint32_t), 0, sizeof(uint32_t));
    *argv_out = (char **)(uintptr_t)(ZZAPP_AS_WINDOW_BASE + argv_offset);
    return ZZ_OK;
}
#else
static int copy_argv(uint32_t argc, char ***argv_out)
{
    uint8_t *window = (uint8_t *)zzapp_services_window();
    char **argv;
    uint32_t i;
    uint32_t total = (argc + 1u) * (uint32_t)sizeof(*argv);

    if (argc > ZZAPP_ARGV_MAX || !g_mem)
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
    if (g_heap_bump + total > g_heap_end)
        return ZZ_ENOMEM;
    argv = (char **)(g_mem + g_heap_bump);
    g_heap_bump += (argc + 1u) * (uint32_t)sizeof(*argv);
    for (i = 0; i < argc; i++) {
        uint32_t off = zzapp_get_be32(window + ZZAPP_ARGV_OFF + ZZAPP_ARGV_OFFS_OFF + 4u * i);
        char *dst = (char *)(g_mem + g_heap_bump);
        uint32_t n = 0;
        while (window[off + n]) { dst[n] = (char)window[off + n]; n++; }
        dst[n++] = '\0';
        argv[i] = dst;
        g_heap_bump += n;
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
    char **argv = NULL;
    int rc;

    if (!g_image_loaded || !g_mem) { start_failed(ZZ_EINVAL); return; }
    g_heap_off = (g_image_end + 7u) & ~7u;
#ifdef ZZAPP_HOST_TEST
    g_heap_end = ZZAPP_ARENA_BYTES - g_stack_bytes;
#else
    g_heap_end = g_proc[0].heap_limit;
#endif
    g_heap_bump = g_heap_off;
    if (g_heap_end <= g_heap_off) { start_failed(ZZ_ENOMEM); return; }
    rc = copy_argv(argc, &argv);
    if (rc != ZZ_OK) { start_failed(rc); return; }
    /* argv is app-owned memory. Newlib's _sbrk begins immediately after it. */
    g_heap_off = g_heap_bump;
    g_proc[0].heap_base = g_heap_off;
    g_proc[0].heap_break = g_heap_off;
    g_proc[0].heap_limit = g_heap_end;
#ifndef ZZAPP_HOST_TEST
    if (!protect_firmware_text()) {
        start_failed(ZZ_EIO);
        return;
    }
#endif
#ifdef ZZAPP_HOST_TEST
    memset(g_mem + g_heap_end, 0xA5, ZZAPP_CANARY_BYTES);
#else
    {
        uint8_t *canary = proc_alias(g_heap_end, ZZAPP_CANARY_BYTES);

        if (!canary) { start_failed(ZZ_ENOMEM); return; }
        memset(canary, 0xA5, ZZAPP_CANARY_BYTES);
    }
#endif
    g_stop_mark = 0;
    g_fault_mark = 0;
    g_faulted = 0;
    g_exit_code = 0;
    /* Core 0's watchdog and lease checks trust these stamps whenever it sees
     * running == 1, so they must be fresh before running is published. The
     * first app after boot otherwise met stamps from power-on (0) and was
     * cold-restarted while starting. */
    g_coh->heartbeat_value = life_get32(ZZAPP_LIFE_HEARTBEAT_OFF);
    g_coh->heartbeat_seen = now_ticks();
    g_coh->tick_seen = now_ticks();
    zzapp_note_progress();
    begin_tick_observation();
    coherent_barrier();
    if (zzapp_kernel_prepare_app(&g_kernel, 0, g_card + g_entry,
                                 (uint32_t)(uintptr_t)zzapp_api_table(), argc,
                                 (uint32_t)(uintptr_t)argv,
                                 g_card + g_proc[0].stack_top) != 0) {
        start_failed(ZZ_EINVAL);
        return;
    }
    g_coh->running = 1;
    publish_state(ZZAPP_ST_RUNNING);
    zzapp_services_start_entered(ZZ_OK);
    (void)zzapp_kernel_start_app(&g_kernel, 0);
#ifdef ZZAPP_HOST_TEST
    zzapp_kernel_set_current_thread(0);
    zzapp_private_timer_enable();
    if (g_test_entry)
        g_test_entry(zzapp_api_table(), (int)argc, argv);
    zzapp_private_timer_disable();
    zzapp_kernel_set_current_thread(-1);
    g_coh->running = 0;
    g_watchdog_reason = 0u;
    if (!g_faulted) {
        life32(ZZAPP_LIFE_EXIT_OFF, g_exit_code);
        zzapp_services_on_app_end();
        zzapp_minigl_reclaim(g_coh->generation);
        publish_state(ZZAPP_ST_IDLE);
        zzapp_loader_release();
    }
#else
    /*
     * The worker remains the system thread. Its next SVC saves that context
     * and returns through the synthetic app frame above; run_app never nests
     * the app below the worker stack.
     */
    zzapp_kernel_set_current_thread(-1);
    zzapp_private_timer_enable();
    return;
#endif
}

void zzapp_worker_hook(void)
{
    uint32_t argc = 0;

    if (g_coh->reset_request) {
        if (g_state == ZZAPP_ST_LOADING || g_state == ZZAPP_ST_STARTING) {
            zzapp_services_cancel_start(ZZ_EIO);
            (void)zzapp_loader_abort();
        } else if (g_coh->running) {
            force_stop(ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
        }
        if (g_state == ZZAPP_ST_FAULTED) {
            zzapp_loader_release();
            publish_state(ZZAPP_ST_IDLE);
        }
        zzapp_services_on_reset();
        g_stop_mark = 0;
        g_fault_mark = 0;
        coherent_barrier();
        g_coh->reset_ack = 1;
        g_coh->reset_request = 0;
        coherent_publish();
        return;
    }
    zzapp_services_poll();
    if (g_state == ZZAPP_ST_LOADING && stop_requested()) {
        zzapp_services_cancel_start(ZZ_EIO);
        (void)zzapp_loader_abort();
        return;
    }
    if (g_state == ZZAPP_ST_LOADING && zzapp_services_take_start(&argc)) {
        publish_state(ZZAPP_ST_STARTING);
        run_app(argc);
    }
}

int zzapp_fault_is_app(uint32_t pc, uint32_t abort_lr)
{
#ifdef ZZAPP_HOST_TEST
    uint32_t end = g_card + ZZAPP_ARENA_BYTES;
    return g_coh->running && g_card && end > g_card &&
           ((pc >= g_card && pc < end) || (abort_lr >= g_card && abort_lr < end));
#else
    (void)pc;
    (void)abort_lr;
    return g_coh->running && zzapp_kernel_current_thread() >= 0;
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
    /* Pairs with run_app's barrier: stamps are fresh once running reads 1. */
    uint32_t running = g_coh->running;
    uint64_t age;
    uint64_t tick_age;
    uint64_t now;
    uint32_t heartbeat;

    coherent_barrier();
    age = core1_stamp_age_us(&g_coh->progress);
    tick_age = core1_stamp_age_us(&g_coh->tick_seen);
    now = now_ticks();
    heartbeat = life_get32(ZZAPP_LIFE_HEARTBEAT_OFF);

    maybe_expire_fault(now);
    if (heartbeat != g_coh->heartbeat_value) {
        g_coh->heartbeat_value = heartbeat;
        g_coh->heartbeat_seen = now;
    }
    /* Heartbeat is an Amiga clock value; only its change is meaningful here. */
    if (running && g_state == ZZAPP_ST_RUNNING &&
        core1_stamp_age_us(&g_coh->heartbeat_seen) >= ZZAPP_LEASE_US)
        life32(ZZAPP_LIFE_STOP_OFF, 1u);
    if (running && age >= ZZAPP_WATCHDOG_US &&
        tick_age >= ZZAPP_WATCHDOG_US) {
        /* Do not reclaim app state until the stopped worker cannot own a lock. */
        g_coh->running = 0;
        g_coh->fault = ZZAPP_FAULT_WATCHDOG_RESTART;
        life32(ZZAPP_LIFE_FAULT_OFF, ZZAPP_FAULT_WATCHDOG_RESTART);
        (void)claim_fault();
        g_fault_mark = now_ticks();
        publish_state(ZZAPP_ST_FAULTED);
        g_cold_restarts++;
#ifndef ZZAPP_HOST_TEST
        printf("[app] watchdog restart: no progress for %lu ms, no tick for "
               "%lu ms, in_api=%lu\r\n", (unsigned long)(age / 1000u),
               (unsigned long)(tick_age / 1000u),
               (unsigned long)g_coh->in_api);
        core1_cold_restart();
#endif
        zzapp_services_on_app_end();
        zzapp_minigl_reclaim(g_coh->generation);
#ifndef ZZAPP_HOST_TEST
        if (g_owner)
            card_pool_release_owner(&card_pool, g_owner);
#endif
        if (zzapp_api_poison_resources)
            zzapp_api_poison_resources();
        zzapp_loader_unbound();
    }
}

void zzapp_on_core1_restart(void)
{
    if (g_coh->running) {
        force_stop(ZZAPP_FAULT_WATCHDOG_RESTART, 0, 0, 0, 0, 0, 0);
        g_cold_restarts++;
    }
}

/* A running app stops itself within one private-timer tick and the worker
 * acks after its teardown.  Bounded well under the 2 s API-stall limit. */
#define ZZAPP_RESET_ACK_WAIT_US 1000000u

void zzapp_on_amiga_reset(void)
{
    uint32_t running = g_coh->running;

    /* Core 1 owns deferred operations and their completion publication.
     * Publish stop first; worker_hook performs service teardown and acks. */
    coherent_barrier();
    g_coh->reset_ack = 0;
    g_coh->reset_request = 1;
    coherent_publish();
    wake_core1();
#ifndef ZZAPP_HOST_TEST
    /*
     * The reset path continues with the core-1 quiesce, the L1 clean
     * request and the Fast-RAM take-back.  A running app answers none of
     * those, so without this wait every Amiga reset during an app
     * cold-restarted a healthy core 1, which can wedge the Zorro bus.
     * Only a core 1 that misses the ack still takes the restart.
     */
    if (running) {
        uint64_t start = now_ticks();

        while (!g_coh->reset_ack &&
               ticks_to_us(now_ticks() - start) < ZZAPP_RESET_ACK_WAIT_US)
            usleep(100u);
        if (!g_coh->reset_ack)
            printf("[app] reset: core 1 did not stop the app within 1 s\r\n");
    }
#else
    (void)running;
#endif
}

void zzapp_status_observed(void)
{
    if (g_state == ZZAPP_ST_FAULTED)
        publish_state(ZZAPP_ST_IDLE);
}

int zzapp_flip_ack_is_current(uint32_t gen)
{
    return life_get32(ZZAPP_LIFE_FLIP_ACK_OFF) == gen;
}

void zzapp_boot_init(void)
{
    memset((void *)g_coh, 0, sizeof(*g_coh));
    memset(g_proc, 0, sizeof(g_proc));
    zzapp_kernel_init(&g_kernel);
    zzapp_loader_memory_init();
    g_state = ZZAPP_ST_IDLE;
    g_faulted = 0;
    g_stop_mark = 0;
    g_fault_mark = 0;
    g_cold_restarts = 0;
#ifdef ZZAPP_HOST_TEST
    g_test_service_calls = 0;
#endif
}

void zzapp_host_exit(int code)
{
    g_exit_code = (uint32_t)code;
#ifndef ZZAPP_HOST_TEST
    if (zzapp_kernel_current_thread() >= 0) {
        zzapp_kernel_stop_app(&g_kernel, current_slot());
        g_coh->running = 0;
        life32(ZZAPP_LIFE_EXIT_OFF, g_exit_code);
        zzapp_services_on_app_end();
        zzapp_minigl_reclaim(g_coh->generation);
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
    g_host_visible_commit_active = 1u;
}

void zzapp_host_commit_end(void)
{
    g_host_visible_commit_active = 0u;
}

void *zzapp_host_alloc(uint32_t bytes, uint32_t align, uint32_t flags)
{
    uint32_t i;

    if ((flags & ~(ZZ_ALLOC_UNCACHED | ZZ_ALLOC_HOST_VISIBLE)) != 0u ||
        !bytes || align < 8u || (align & (align - 1u)) != 0u)
        return NULL;
    /*
     * The arena heap belongs to the app's newlib (_sbrk over heap_range),
     * which the runtime cannot see into. A second allocator here would hand
     * out bytes malloc already owns, so the table allocates host-visible
     * memory only; zz_alloc serves arena requests from malloc.
     */
    if ((flags & ZZ_ALLOC_HOST_VISIBLE) == 0u)
        return NULL;
    if (bytes > ZZAPP_HOST_VISIBLE_LIMIT - g_host_visible_used)
        return NULL;
    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i) {
        uint32_t card;
        void *ptr;

        if (g_host_visible[i].ptr)
            continue;
        /*
         * surface_allocator_* restores interrupts before returning. Keep a
         * forced-stop tick from unwinding the app until this real allocation
         * has a ledger entry that teardown can release.
         */
        zzapp_host_commit_begin();
#ifndef ZZAPP_HOST_TEST
        card = surface_allocator_alloc_aligned(bytes, align);
        ptr = (void *)(uintptr_t)card;
#else
        ptr = malloc(bytes);
        card = 0x033f0000u + g_host_visible_used;
        extern void zzapp_test_host_visible_allocated(void) __attribute__((weak));
        if (zzapp_test_host_visible_allocated)
            zzapp_test_host_visible_allocated();
#endif
        if (!ptr) {
            g_host_visible_commit_active = 0u;
            return NULL;
        }
        g_host_visible[i].ptr = ptr;
        g_host_visible[i].card = card;
        g_host_visible[i].bytes = bytes;
        g_host_visible_used += bytes;
        g_host_visible_commit_active = 0u;
        return ptr;
    }
    return NULL;
}

void zzapp_host_free(void *p)
{
    uint32_t i;

    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i) {
        if (g_host_visible[i].ptr != p)
            continue;
        g_host_visible_commit_active = 1u;
#ifndef ZZAPP_HOST_TEST
        (void)surface_allocator_free(g_host_visible[i].card);
#else
        free(p);
#endif
        g_host_visible_used -= g_host_visible[i].bytes;
        g_host_visible[i].ptr = NULL;
        g_host_visible_commit_active = 0u;
        return;
    }
}

void zzapp_host_heap(void **base, uint32_t *bytes)
{
    if (base)
        *base = g_mem ? g_mem + g_heap_off : NULL;
    if (bytes)
        *bytes = g_heap_end > g_heap_off ? g_heap_end - g_heap_off : 0;
}

int zzapp_host_address(const void *p, uint32_t *host_offset)
{
    uint32_t i;

    if (!host_offset)
        return ZZ_EINVAL;
    for (i = 0; i < ZZAPP_HOST_VISIBLE_SLOTS; ++i) {
        if (g_host_visible[i].ptr == p) {
#ifndef ZZAPP_HOST_TEST
            *host_offset = g_host_visible[i].card - ADDR_ADJ;
#else
            *host_offset = g_host_visible[i].card;
#endif
            return ZZ_OK;
        }
    }
    return ZZ_EINVAL;
}

int zzapp_host_mem_info(zzapp_mem_info *out)
{
    if (!out)
        return ZZ_EINVAL;
    out->arena_size = g_mem ? ZZAPP_ARENA_BYTES : 0u;
    out->arena_used = g_heap_bump > g_heap_off ? g_heap_bump - g_heap_off : 0u;
    out->arena_free = g_heap_end > g_heap_bump ? g_heap_end - g_heap_bump : 0u;
    out->arena_largest_block = out->arena_free;
    out->host_visible_used = g_host_visible_used;
    out->host_visible_limit = ZZAPP_HOST_VISIBLE_LIMIT;
    return ZZ_OK;
}
#ifdef ZZAPP_HOST_TEST
void zzapp_test_set_xtime(uint64_t ticks) { g_test_ticks = ticks; }
void zzapp_test_set_entry(void (*fn)(const void *api, int argc, char **argv)) { g_test_entry = fn; }
void zzapp_host_tick_at(uint64_t now) { g_test_ticks = now; service_tick(now, 0); }
void zzapp_test_mark_running(void) { g_coh->running = 1; g_state = ZZAPP_ST_RUNNING; zzapp_note_progress(); begin_tick_observation(); }
uint32_t zzapp_test_service_calls(void) { return g_test_service_calls; }
uint32_t zzapp_test_in_api(void) { return g_coh->in_api; }

int zzapp_test_cold_restarts(void) { return g_cold_restarts; }
uint32_t zzapp_test_exit_code(void) { return g_exit_code; }
uint8_t *zzapp_test_stack_canary(void)
{
    return (g_mem && g_heap_end != 0u) ? g_mem + g_heap_end : 0;
}
uint32_t zzapp_test_fault_code(void) { return g_coh->fault; }
#endif
