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
void scheduler_core1_service_once(void) { g_test_service_calls++; }
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
#ifndef ZZAPP_HOST_TEST
/* Set while a task-context service tick runs (API epilogue on app_host_sp,
 * or a yield on the app stack).  The private-timer IRQ then neither nests
 * service_tick nor switches to app_host_sp.  Global: the IRQ entry reads it
 * from assembly. */
volatile int g_host_tick_active;
static volatile int g_unwind_pending;
#endif

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
/* Saved SVC stack below the worker's app-entry VFP frame. Every app service
 * tick runs on this firmware stack, never on the app arena or IRQ stack. */
volatile uint32_t app_host_sp;
static int g_private_timer_ready;
static int g_firmware_text_readonly;
extern char __firmware_text_start[];
extern char __firmware_text_end[];

__attribute__((naked)) void zzapp_enter_arm(uint32_t entry, const void *api,
                                            uint32_t argc, char **argv,
                                            uint32_t sp)
{
    (void)entry; (void)api; (void)argc; (void)argv; (void)sp;
    __asm__ volatile (
        "push {r4-r11, lr}\n"
        "vmrs r4, fpscr\n"
        "push {r4}\n"
        "vpush {d0-d15}\n"
        "vpush {d16-d31}\n"
        /* App ABI baseline: round-to-nearest, no FZ and no default-NaN. */
        "mov r4, #0\n"
        "vmsr fpscr, r4\n"
        "ldr r4, [sp, #296]\n"
        "ldr r12, =app_host_sp\n"
        "str sp, [r12]\n"
        "mov r12, r0\n"
        "mov r0, r1\n"
        "mov r1, r2\n"
        "mov r2, r3\n"
        "mov sp, r4\n"
        "blx r12\n"
        "ldr r12, =app_host_sp\n"
        "ldr sp, [r12]\n"
        "vpop {d16-d31}\n"
        "vpop {d0-d15}\n"
        "pop {r4}\n"
        "vmsr fpscr, r4\n"
        "pop {r4-r11, lr}\n"
        "bx lr\n");
}

/* Return to run_app through the saved zzapp_enter_arm frame.  This is used
 * for normal exit, watchdog/stop unwinds, and app exceptions; no app frame
 * survives any of those paths. */
extern void core1_reset_exception_stacks(void);
__attribute__((naked, noreturn)) static void zzapp_unwind_to_host(void)
{
    __asm__ volatile (
        "cpsid if\n"
        "bl core1_reset_exception_stacks\n"
        "cps #0x13\n"
        "ldr r12, =app_host_sp\n"
        "ldr sp, [r12]\n"
        "vpop {d16-d31}\n"
        "vpop {d0-d15}\n"
        "pop {r4}\n"
        "vmsr fpscr, r4\n"
        "pop {r4-r11, lr}\n"
        "cpsie i\n"
        "bx lr\n");
}

static void zzapp_unwind_if_needed(void)
{
    if (g_unwind_pending)
        zzapp_unwind_to_host();
}

__attribute__((naked, noreturn)) static void zzapp_resume_worker_from_fault(void)
{
    __asm__ volatile (
        "b zzapp_unwind_to_host\n");
}
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
    host_visible_release_all();
    if (zzapp_api_reclaim_resources)
        zzapp_api_reclaim_resources();
    g_card = 0;
    g_mem = NULL;
    g_owner = 0;
    g_entry = 0;
    g_image_loaded = 0;
    g_image_end = 0;
    g_stack_bytes = 0;
    g_coh->arena_card = 0;
    g_coh->arena_bytes = 0;
    g_coh->owner = 0;
}

uint32_t zzapp_loader_generation(void) { return g_coh->generation; }
uint32_t zzapp_loader_state(void) { return g_state; }
void zzapp_loader_set_state(uint32_t state) { publish_state(state); }
uint32_t zzapp_loader_reset_requested(void) { return g_coh->reset_request; }
int zzapp_loader_have_image(void) { return g_image_loaded; }

void zzapp_loader_set_image(uint32_t entry, uint32_t image_end,
                            uint32_t stack_bytes, uint32_t note_flags)
{
    g_entry = entry;
    g_image_loaded = 1;
    g_image_end = image_end;
    g_stack_bytes = stack_bytes;
    life32(ZZAPP_LIFE_FLAGS_OFF,
           note_flags & (ZZAPP_F_DISPLAY | ZZAPP_F_AUDIO | ZZAPP_F_GL));
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
    g_unwind_pending = 1;
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

static void service_tick(uint64_t now, int permit_reentry)
{
    uint32_t calls = g_coh->api_calls;

    g_coh->tick_seen = now;
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
    zzapp_services_poll();
    /*
     * A tick must not acquire MiniGL or SDK work below an app API call.
     * File waits and sleep explicitly pass permit_reentry between polls.
     */
    if (g_coh->in_api == 0u || permit_reentry) {
        g_coh->service_due = 0u;
        scheduler_core1_service_once();
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
static void zzapp_private_timer_eoi(void)
{
    XScuGic_CPUWriteReg(interrupt_get_intc(), XSCUGIC_EOI_OFFSET,
                        XPAR_PS7_SCUTIMER_0_INTR);
    __asm__ __volatile__("dsb sy" ::: "memory");
}

static __attribute__((noinline, used)) void zzapp_private_timer_service(void)
{
    Xil_Out32(XPAR_PS7_SCUTIMER_0_BASEADDR + XSCUTIMER_ISR_OFFSET,
              XSCUTIMER_ISR_EVENT_FLAG_MASK);
    if (g_coh->running) {
        if (!g_host_tick_active)
            service_tick(now_ticks(), 0);
        else
            /* A task-context tick is still running (possibly a long SDK
             * task); core 1 is alive and taking interrupts. */
            g_coh->tick_seen = now_ticks();
    }
    if (g_unwind_pending) {
        /* We discard the IRQ frame below, so acknowledge the GIC before
         * restoring run_app's saved worker context. */
        zzapp_private_timer_eoi();
        zzapp_unwind_if_needed();
    }
}

/* The BSP vector saves d0-d7/d16-d31 and FPSCR. Preserve the remaining
 * d8-d15 before switching from its 8 KiB IRQ stack to app_host_sp.  While a
 * task-context tick is active, app_host_sp is already in use by
 * zzapp_api_tick_body; switching to it would overwrite that live frame
 * (its saved return address came back as 0x42a00028 on hardware), so stay
 * on the IRQ stack: the service routine then does no deep work. */
__attribute__((naked)) static void zzapp_private_timer_irq(void *unused)
{
    (void)unused;
    __asm__ volatile (
        "push {r4, lr}\n"
        "vpush {d8-d15}\n"
        "mov r4, sp\n"
        "ldr r12, =g_host_tick_active\n"
        "ldr r12, [r12]\n"
        "cmp r12, #0\n"
        "bne 1f\n"
        "ldr r12, =app_host_sp\n"
        "ldr sp, [r12]\n"
        "1:\n"
        "bl zzapp_private_timer_service\n"
        "mov sp, r4\n"
        "vpop {d8-d15}\n"
        "pop {r4, pc}\n");
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
#endif

#ifndef ZZAPP_HOST_TEST
/* A service tick from task context.  The flag keeps the private-timer IRQ
 * from running a nested service_tick on top of this one. */
static void service_tick_in_task(uint64_t now, int permit_reentry)
{
    int outer = g_host_tick_active;

    g_host_tick_active = 1;
    __asm__ __volatile__("" ::: "memory");
    service_tick(now, permit_reentry);
    __asm__ __volatile__("" ::: "memory");
    g_host_tick_active = outer;
}
#else
#define service_tick_in_task service_tick
#endif

void zzapp_host_yield(void)
{
    zzapp_note_progress();
    service_tick_in_task(now_ticks(), 0);
#ifndef ZZAPP_HOST_TEST
    zzapp_unwind_if_needed();
#endif
}

void zzapp_host_yield_wait(void)
{
    uint64_t now = now_ticks();

    /*
     * This is a declared blocking service wait (display/file/timeout), not
     * an API that wedged in host code.  Renew the API-stall deadline before
     * servicing the wait so a delayed launcher acknowledgement cannot turn
     * into a false watchdog fault.
     */
    if (g_coh->in_api != 0u)
        g_coh->api_entered = now;
    zzapp_note_progress();
    service_tick_in_task(now, 1);
#ifndef ZZAPP_HOST_TEST
    zzapp_unwind_if_needed();
#endif
}

uint64_t zzapp_host_time_us(void) { return ticks_to_us(now_ticks()); }

#ifndef ZZAPP_HOST_TEST
static __attribute__((noinline, used)) void zzapp_api_tick_body(void)
{
    service_tick(now_ticks(), 0);
    /* g_host_tick_active stays set: this frame on app_host_sp is live until
     * the wrapper has switched back, and an IRQ must not reuse that stack. */
    zzapp_unwind_if_needed();
}

__attribute__((naked)) static void zzapp_api_tick_on_host_stack(void)
{
    __asm__ volatile (
        "push {r4, lr}\n"
        "vmrs r4, fpscr\n"
        "push {r4}\n"
        "vpush {d0-d15}\n"
        "vpush {d16-d31}\n"
        "mov r4, sp\n"
        "ldr r12, =app_host_sp\n"
        "ldr sp, [r12]\n"
        "bl zzapp_api_tick_body\n"
        "mov sp, r4\n"
        "vpop {d16-d31}\n"
        "vpop {d0-d15}\n"
        "pop {r4}\n"
        "vmsr fpscr, r4\n"
        "pop {r4, pc}\n");
}
#endif

/*
 * Every GL call of an ARM app passes through here (GLQuake makes tens of
 * thousands per frame), so neither side reads the clock or touches the
 * uncached lifecycle block; service_tick derives the stamps.
 */
void zzapp_api_enter(void)
{
    g_coh->api_calls++;
    g_coh->in_api++;
}

void zzapp_api_leave(void)
{
    if (g_coh->in_api)
        g_coh->in_api--;
    if (!g_coh->in_api && g_coh->service_due) {
#ifdef ZZAPP_HOST_TEST
        service_tick(now_ticks(), 0);
#else
        /* Never nest: an active tick may own app_host_sp. */
        if (!g_host_tick_active) {
            g_host_tick_active = 1;
            zzapp_api_tick_on_host_stack();
            g_host_tick_active = 0;
        }
#endif
    }
}

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
    g_heap_end = ZZAPP_ARENA_BYTES - g_stack_bytes;
    g_heap_bump = g_heap_off;
    if (g_heap_end <= g_heap_off) { start_failed(ZZ_ENOMEM); return; }
    rc = copy_argv(argc, &argv);
    if (rc != ZZ_OK) { start_failed(rc); return; }
    /* argv is app-owned memory. Newlib's _sbrk begins immediately after it. */
    g_heap_off = g_heap_bump;
#ifndef ZZAPP_HOST_TEST
    if (!protect_firmware_text()) {
        start_failed(ZZ_EIO);
        return;
    }
#endif
    memset(g_mem + g_heap_end, 0xA5, ZZAPP_CANARY_BYTES);
    g_stop_mark = 0;
    g_fault_mark = 0;
    g_faulted = 0;
    g_exit_code = 0;
#ifndef ZZAPP_HOST_TEST
    g_unwind_pending = 0;
    g_host_tick_active = 0;
#endif
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
    g_coh->running = 1;
    publish_state(ZZAPP_ST_RUNNING);
    zzapp_services_start_entered(ZZ_OK);
    zzapp_private_timer_enable();
#ifdef ZZAPP_HOST_TEST
    if (g_test_entry)
        g_test_entry(zzapp_api_table(), (int)argc, argv);
#else
    /* The ARM entry trampoline is intentionally separate from the loader.
     * It establishes r0-r2 and the arena stack, then returns only via exit. */
    extern void zzapp_enter_arm(uint32_t entry, const void *api, uint32_t argc,
                                char **argv, uint32_t sp);
    zzapp_enter_arm(g_card + g_entry, zzapp_api_table(), argc, argv,
                    g_card + ZZAPP_ARENA_BYTES);
#endif
    zzapp_private_timer_disable();
#ifndef ZZAPP_HOST_TEST
    /* The unwind (exit, force-stop or fault) has landed here.  A pending
     * flag left set would make any later host yield unwind onto this
     * worker frame again after it has been reused. */
    g_unwind_pending = 0;
#endif
    g_coh->running = 0;
#ifndef ZZAPP_HOST_TEST
    if (g_watchdog_reason == 1u)
        printf("[app] stopped: no app API call for %lu s\r\n",
               (unsigned long)(ZZAPP_APP_STALL_US / 1000000u));
    else if (g_watchdog_reason == 2u)
        printf("[app] stopped: one app API call ran for %lu s\r\n",
               (unsigned long)(ZZAPP_API_STUCK_US / 1000000u));
#endif
    g_watchdog_reason = 0u;
    if (!g_faulted) {
        life32(ZZAPP_LIFE_EXIT_OFF, g_exit_code);
        zzapp_services_on_app_end();
        zzapp_minigl_reclaim(g_coh->generation);
        publish_state(ZZAPP_ST_IDLE);
        zzapp_loader_release();
    }
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
    uint32_t end = g_card + ZZAPP_ARENA_BYTES;
    return g_coh->running && g_card && end > g_card &&
           ((pc >= g_card && pc < end) || (abort_lr >= g_card && abort_lr < end));
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
#ifndef ZZAPP_HOST_TEST
    zzapp_resume_worker_from_fault();
#endif
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
    g_unwind_pending = 1;
    zzapp_unwind_to_host();
#endif
}

void *zzapp_host_alloc(uint32_t bytes, uint32_t align, uint32_t flags)
{
    uintptr_t p;
    uint32_t i;

    if ((flags & ~(ZZ_ALLOC_UNCACHED | ZZ_ALLOC_HOST_VISIBLE)) != 0u ||
        !bytes || align < 8u || (align & (align - 1u)) != 0u)
        return NULL;
    if ((flags & ZZ_ALLOC_HOST_VISIBLE) == 0u) {
        if (!g_mem)
            return NULL;
        p = (uintptr_t)(g_mem + g_heap_bump);
        p = (p + align - 1u) & ~(uintptr_t)(align - 1u);
        if (p + bytes < p || p + bytes > (uintptr_t)(g_mem + g_heap_end))
            return NULL;
        g_heap_bump = (uint32_t)(p + bytes - (uintptr_t)g_mem);
        return (void *)p;
    }
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
        g_host_visible_commit_active = 1u;
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
#endif
