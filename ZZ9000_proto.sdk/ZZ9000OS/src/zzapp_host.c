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
#include "memorymap.h"
#include "xtime_l.h"
#include "xil_cache.h"
#include "card_pool.h"
#include "core2.h"
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
    volatile uint32_t arena_card;
    volatile uint32_t arena_bytes;
    volatile uint32_t owner;
    volatile uint32_t reset_request;
};
#ifdef ZZAPP_HOST_TEST
void scheduler_core1_service_once(void) {}
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
static uint32_t g_image_end;
static uint32_t g_stack_bytes;
static uint32_t g_note_flags;
static uint64_t g_stop_mark;
static uint64_t g_heartbeat_mark;
static uint32_t g_last_heartbeat;
static uint64_t g_fault_mark;
static uint32_t g_exit_code;
static uint32_t g_heap_off;
static uint32_t g_heap_end;
static uint32_t g_heap_bump;
static int g_exited;
static int g_faulted;
static int g_cold_restarts;

#ifdef ZZAPP_HOST_TEST
static void (*g_test_entry)(const void *api, int argc, char **argv);
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
    return ticks * 1000000ULL / (uint64_t)COUNTS_PER_SECOND;
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

static void publish_state(uint32_t state)
{
    g_state = state;
    life32(ZZAPP_LIFE_STATE_OFF, state);
}

void zzapp_loader_bound(uint32_t card, uint8_t *mem, uint32_t bytes,
                        uint32_t owner, uint32_t generation)
{
    g_card = card;
    g_mem = mem;
    g_owner = owner;
    g_coh->arena_card = card;
    g_coh->arena_bytes = bytes;
    g_coh->owner = owner;
    g_coh->generation = generation;
    life32(ZZAPP_LIFE_MAGIC_OFF, ZZAPP_LIFE_MAGIC);
    life32(ZZAPP_LIFE_VERSION_OFF, ZZAPP_LIFE_VERSION);
    life32(ZZAPP_LIFE_GENERATION_OFF, generation);
}

void zzapp_loader_unbound(void)
{
    g_card = 0;
    g_mem = NULL;
    g_owner = 0;
    g_coh->arena_card = 0;
    g_coh->arena_bytes = 0;
    g_coh->owner = 0;
}

uint32_t zzapp_loader_generation(void) { return g_coh->generation; }
uint32_t zzapp_loader_state(void) { return g_state; }
void zzapp_loader_set_state(uint32_t state) { publish_state(state); }
int zzapp_loader_have_image(void) { return g_entry != 0; }

void zzapp_loader_set_image(uint32_t entry, uint32_t image_end,
                            uint32_t stack_bytes, uint32_t note_flags)
{
    g_entry = entry;
    g_image_end = image_end;
    g_stack_bytes = stack_bytes;
    g_note_flags = note_flags;
    life32(ZZAPP_LIFE_FLAGS_OFF, note_flags & (ZZAPP_F_DISPLAY | ZZAPP_F_AUDIO | ZZAPP_F_GL));
}

uint32_t zzapp_current_generation(void) { return g_coh->generation; }
uint32_t zzapp_runtime_state(void) { return g_state; }
uint32_t zzapp_runtime_caps(void) { return ZZAPP_CAP_Z3 | ZZAPP_CAP_DISPLAY | ZZAPP_CAP_AUDIO | ZZAPP_CAP_GL; }

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

static void force_stop(uint32_t fault, uint32_t pc, uint32_t sp,
                       uint32_t lr, uint32_t far, uint32_t fsr, uint32_t cpsr)
{
    if (g_faulted)
        return;
    g_faulted = 1;
    g_coh->running = 0;
    g_coh->fault = fault;
    life32(ZZAPP_LIFE_FAULT_OFF, fault);
    life32(ZZAPP_LIFE_PC_OFF, pc);
    life32(ZZAPP_LIFE_SP_OFF, sp);
    life32(ZZAPP_LIFE_LR_OFF, lr);
    life32(ZZAPP_LIFE_FAR_OFF, far);
    life32(ZZAPP_LIFE_FSR_OFF, fsr);
    life32(ZZAPP_LIFE_CPSR_OFF, cpsr);
    publish_state(ZZAPP_ST_FAULTED);
    g_fault_mark = now_ticks();
    zzapp_gl_reclaim();
#ifndef ZZAPP_HOST_TEST
    if (g_owner)
        card_pool_release_owner(&card_pool, g_owner);
#endif
    zzapp_loader_unbound();
}

static void maybe_expire_fault(uint64_t now)
{
    if (g_state == ZZAPP_ST_FAULTED &&
        ticks_to_us(now - g_fault_mark) >= ZZAPP_WATCHDOG_US)
        publish_state(ZZAPP_ST_IDLE);
}

static void service_tick(uint64_t now, int allow_scheduler)
{
    g_coh->tick_seen = now;
    zzapp_services_poll();
    if (allow_scheduler)
        scheduler_core1_service_once();
    if (stop_requested()) {
        if (!g_stop_mark)
            g_stop_mark = now;
        if (ticks_to_us(now - g_stop_mark) >= ZZAPP_STOP_GRACE_US)
            force_stop(ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
    } else {
        g_stop_mark = 0;
    }
    if (g_coh->running && g_coh->in_api == 0u &&
        ticks_to_us(now - g_coh->progress) >= ZZAPP_WATCHDOG_US)
        force_stop(ZZAPP_FAULT_WATCHDOG, 0, 0, 0, 0, 0, 0);
    maybe_expire_fault(now);
}

void zzapp_host_yield(void)
{
    zzapp_note_progress();
    service_tick(now_ticks(), 1);
}

uint64_t zzapp_host_time_us(void) { return ticks_to_us(now_ticks()); }

void zzapp_api_enter(void)
{
    g_coh->in_api++;
}

void zzapp_api_leave(void)
{
    if (g_coh->in_api)
        g_coh->in_api--;
    if (!g_coh->in_api)
        zzapp_note_progress();
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

static void run_app(uint32_t argc)
{
    char **argv = NULL;
    int rc;

    if (!g_entry || !g_mem) { zzapp_services_start_entered(ZZ_EINVAL); return; }
    g_heap_off = (g_image_end + 7u) & ~7u;
    g_heap_end = ZZAPP_ARENA_BYTES - g_stack_bytes;
    g_heap_bump = g_heap_off;
    if (g_heap_end <= g_heap_off) { zzapp_services_start_entered(ZZ_ENOMEM); return; }
    rc = copy_argv(argc, &argv);
    if (rc != ZZ_OK) { zzapp_services_start_entered(rc); return; }
    memset(g_mem + g_heap_end, 0xA5, ZZAPP_CANARY_BYTES);
    g_exited = 0;
    g_faulted = 0;
    g_exit_code = 0;
    g_coh->running = 1;
    zzapp_note_progress();
    publish_state(ZZAPP_ST_RUNNING);
    zzapp_services_start_entered(ZZ_OK);
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
    g_coh->running = 0;
    if (!g_faulted) {
        life32(ZZAPP_LIFE_EXIT_OFF, g_exit_code);
        publish_state(ZZAPP_ST_IDLE);
        zzapp_loader_release();
    }
}

void zzapp_worker_hook(void)
{
    uint32_t argc = 0;

    zzapp_services_poll();
    if (g_state == ZZAPP_ST_LOADING && stop_requested()) {
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
}

void zzapp_core0_poll(void)
{
    uint64_t now = now_ticks();
    uint64_t age = ticks_to_us(now - g_coh->progress);
    uint64_t tick_age = ticks_to_us(now - g_coh->tick_seen);

    maybe_expire_fault(now);
    if (g_coh->running && age >= ZZAPP_WATCHDOG_US &&
        tick_age >= ZZAPP_WATCHDOG_US) {
        force_stop(ZZAPP_FAULT_WATCHDOG_RESTART, 0, 0, 0, 0, 0, 0);
        g_cold_restarts++;
#ifndef ZZAPP_HOST_TEST
        core1_cold_restart();
#endif
    }
}

void zzapp_on_core1_restart(void)
{
    if (g_coh->running) {
        force_stop(ZZAPP_FAULT_WATCHDOG_RESTART, 0, 0, 0, 0, 0, 0);
        g_cold_restarts++;
    }
}

void zzapp_on_amiga_reset(void)
{
    g_coh->reset_request = 1;
    if (g_state == ZZAPP_ST_LOADING || g_state == ZZAPP_ST_STARTING)
        (void)zzapp_loader_abort();
    if (g_coh->running)
        force_stop(ZZAPP_FAULT_FORCED, 0, 0, 0, 0, 0, 0);
    zzapp_services_on_reset();
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
}

void zzapp_host_exit(int code)
{
    g_exit_code = (uint32_t)code;
    g_exited = 1;
}

void *zzapp_host_alloc(uint32_t bytes, uint32_t align, uint32_t flags)
{
    uintptr_t p;
    (void)flags;
    if (!g_mem || align < 8u || (align & (align - 1u)) != 0u)
        return NULL;
    p = (uintptr_t)(g_mem + g_heap_bump);
    p = (p + align - 1u) & ~(uintptr_t)(align - 1u);
    if (p + bytes < p || p + bytes > (uintptr_t)(g_mem + g_heap_end))
        return NULL;
    g_heap_bump = (uint32_t)(p + bytes - (uintptr_t)g_mem);
    return (void *)p;
}

void zzapp_host_free(void *p) { (void)p; }

void zzapp_host_heap(void **base, uint32_t *bytes)
{
    if (base)
        *base = g_mem ? g_mem + g_heap_off : NULL;
    if (bytes)
        *bytes = g_heap_end > g_heap_off ? g_heap_end - g_heap_off : 0;
}
#ifdef ZZAPP_HOST_TEST
void zzapp_test_set_xtime(uint64_t ticks) { g_test_ticks = ticks; }
void zzapp_test_set_entry(void (*fn)(const void *api, int argc, char **argv)) { g_test_entry = fn; }
void zzapp_host_tick_at(uint64_t now) { g_test_ticks = now; service_tick(now, 0); }
void zzapp_test_mark_running(void) { g_coh->running = 1; g_state = ZZAPP_ST_RUNNING; zzapp_note_progress(); }

int zzapp_test_cold_restarts(void) { return g_cold_restarts; }
uint32_t zzapp_test_exit_code(void) { return g_exit_code; }
#endif
