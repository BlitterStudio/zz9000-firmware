/*
 * App SVC veneer tables and checked dispatch.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "zzapp_gl_contract.h"
#include "zzapp_runtime.h"
#include "zzapp_services.h"
#include "zzapp_svc.h"
#define L1_TYPE_MASK 3U
#define L1_SECTION 2U
#define L1_TABLE 1U
#define L1_AP_MASK (3U << 10)
#define L2_TYPE_MASK 3U
#define L2_LARGE 1U
#define L2_AP_MASK (3U << 4)
#define PAGE_BYTES 4096U
#define VENEER_BYTES 12U
#define VENEER_PAGE_BYTES 4096U
#define STRING_BYTES 512U
#define COPY_BYTES sizeof(zzapp_dirent)
#define API_FIRST offsetof(zzapp_api, exit)
#define GL_FIRST offsetof(zzapp_gl, Begin)
#define API_SLOT(field) ((uint32_t)((offsetof(zzapp_api, field) - API_FIRST) / sizeof(void *)))
#define GL_SLOT(field) ((uint32_t)((offsetof(zzapp_gl, field) - GL_FIRST) / sizeof(void *)))
#define API_COUNT ((uint32_t)((sizeof(zzapp_api) - API_FIRST) / sizeof(void *)))
#define GL_COUNT ((uint32_t)((sizeof(zzapp_gl) - GL_FIRST) / sizeof(void *)))
#define GL_INVALID_VALUE 0x0501U
#define GL_INVALID_OPERATION 0x0502U
#define GL_BYTE 0x1400U
#define GL_UNSIGNED_BYTE 0x1401U
#define GL_SHORT 0x1402U
#define GL_UNSIGNED_SHORT 0x1403U
#define GL_INT 0x1404U
#define GL_UNSIGNED_INT 0x1405U
#define GL_FLOAT 0x1406U
#define GL_DOUBLE 0x140aU
#define GL_VERTEX_ARRAY 0x8074U
#define MGL_ARRAY_TRANSFORMATIONS 0x701dU
#define GL_COLOR_ARRAY 0x8076U
#define GL_TEXTURE_COORD_ARRAY 0x8078U
#define GL_TEXTURE0 0x84c0U
#define GL_TEXTURE_UNITS 8U

#if ZZAPP_PERF_COUNTERS
#include <stdio.h>
#endif

#ifdef ZZAPP_HOST_TEST
static uint8_t host_veneer_page[VENEER_PAGE_BYTES]
    __attribute__((aligned(VENEER_PAGE_BYTES)));
#define VENEER_PAGE host_veneer_page
#else
extern uint8_t zzapp_svc_veneer_page[];
#define VENEER_PAGE zzapp_svc_veneer_page
#endif

#ifdef ZZAPP_HOST_TEST
__attribute__((weak)) int zzapp_kernel_current_thread(void) { return -1; }
__attribute__((weak)) zzapp_proc_t *zzapp_proc_slot(uint32_t slot)
{ (void)slot; return NULL; }
__attribute__((weak)) const zzapp_api *zzapp_api_table(void) { return NULL; }
#endif

struct user_api_table {
    zzapp_api table;
    char build_id[128];
} __attribute__((aligned(VENEER_PAGE_BYTES)));
struct user_gl_table {
    zzapp_gl table;
    char build_id[128];
} __attribute__((aligned(VENEER_PAGE_BYTES)));
static struct user_api_table user_api[ZZAPP_AS_SLOT_COUNT];
static struct user_gl_table user_gl[ZZAPP_AS_SLOT_COUNT];
static const zzapp_gl *direct_gl[ZZAPP_AS_SLOT_COUNT];
static uintptr_t veneer_alias[ZZAPP_AS_SLOT_COUNT];
static uintptr_t api_alias[ZZAPP_AS_SLOT_COUNT];

typedef struct {
    uint64_t sum;
    uint32_t max;
    uint32_t calls;
} zzapp_svc_cost_t;

typedef struct {
    zzapp_svc_cost_t entry_to_dispatch;
    zzapp_svc_cost_t dispatch_to_handler;
    zzapp_svc_cost_t handler;
    zzapp_svc_cost_t handler_to_eret;
    uint32_t dispatch_stamp;
    uint32_t handler_stamp;
    uint32_t handler_done_stamp;
    uint8_t handler_started;
    uint8_t reported;
    uint32_t total_calls;
} zzapp_svc_counters_t;
_Static_assert(sizeof(zzapp_svc_cost_t) == 16U, "svc cost layout");
_Static_assert(offsetof(zzapp_svc_counters_t, handler_to_eret) == 48U,
               "svc h2e offset");
_Static_assert(offsetof(zzapp_svc_counters_t, handler_done_stamp) == 72U,
               "svc h2e stamp");
_Static_assert(offsetof(zzapp_svc_counters_t, handler_started) == 76U,
               "svc h2e started");
_Static_assert(offsetof(zzapp_svc_counters_t, total_calls) == 80U,
               "svc total calls");
_Static_assert(sizeof(zzapp_svc_counters_t) == 88U, "svc counters layout");


#if ZZAPP_PERF_COUNTERS
volatile uint32_t zzapp_svc_entry_stamp[ZZAPP_AS_SLOT_COUNT];
/* The vector folds the final stage into the selected class without a C call. */
zzapp_svc_counters_t zzapp_svc_counters[ZZAPP_AS_SLOT_COUNT][2];
volatile uint8_t zzapp_svc_counter_class[ZZAPP_AS_SLOT_COUNT];
static inline uint32_t pmccntr(void)
{
#ifdef ZZAPP_SVC_COUNTERS_TEST
    extern uint32_t zzapp_svc_test_pmccntr(void);

    return zzapp_svc_test_pmccntr();
#else
    uint32_t cycles;

    __asm__ __volatile__("mrc p15, 0, %0, c9, c13, 0" : "=r"(cycles));
    return cycles;
#endif
}

static inline void counter_record(zzapp_svc_cost_t *cost, uint32_t cycles)
{
    cost->sum += cycles;
    if (cycles > cost->max)
        cost->max = cycles;
}

static inline void counter_add(zzapp_svc_cost_t *cost, uint32_t cycles)
{
    cost->calls++;
    counter_record(cost, cycles);
}

static inline void counters_dispatch(uint32_t slot, int gl)
{
    zzapp_svc_counters_t *counters = &zzapp_svc_counters[slot][gl != 0];
    uint32_t stamp;

    zzapp_svc_counter_class[slot] = gl != 0;
    counters->total_calls++;
    counters->handler_started = 0U;
    if ((counters->total_calls & 63U) != 0U)
        return;
    stamp = pmccntr();
    counter_add(&counters->entry_to_dispatch,
                stamp - zzapp_svc_entry_stamp[slot]);
    counters->dispatch_stamp = stamp;
}

int zzapp_svc_counters_sampled(uint32_t slot)
{
	zzapp_svc_counters_t *counters;

	if (slot >= ZZAPP_AS_SLOT_COUNT)
		return 0;
	counters = &zzapp_svc_counters[slot][zzapp_svc_counter_class[slot]];
	return (counters->total_calls & 63U) == 0U;
}

static inline void counters_handler_start(uint32_t slot)
{
    zzapp_svc_counters_t *counters =
        &zzapp_svc_counters[slot][zzapp_svc_counter_class[slot]];
    uint32_t stamp;

    if ((counters->total_calls & 63U) != 0U)
        return;
    stamp = pmccntr();
    counter_add(&counters->dispatch_to_handler,
                stamp - counters->dispatch_stamp);
    counters->handler_stamp = stamp;
    counters->handler_started = 1U;
}
static inline void counters_handler_end(uint32_t slot)
{
    zzapp_svc_counters_t *counters =
        &zzapp_svc_counters[slot][zzapp_svc_counter_class[slot]];
    uint32_t stamp;

    if ((counters->total_calls & 63U) != 0U)
        return;
    stamp = pmccntr();
    counter_add(&counters->handler, stamp - counters->handler_stamp);
    counters->handler_done_stamp = stamp;
}

void zzapp_svc_counters_reset(uint32_t slot)
{
    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return;
    memset(zzapp_svc_counters[slot], 0, sizeof(zzapp_svc_counters[slot]));
    zzapp_svc_counter_class[slot] = 0U;
    zzapp_svc_gl_telemetry_reset(slot);
}


static uint32_t counter_average(const zzapp_svc_cost_t *cost, uint32_t samples)
{
    return samples ? (uint32_t)(cost->sum / samples) : 0U;
}

void zzapp_svc_counters_report(uint32_t slot)
{
    uint32_t kind;

    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return;
    for (kind = 0U; kind < 2U; ++kind) {
        zzapp_svc_counters_t *counters = &zzapp_svc_counters[slot][kind];

        if (counters->reported || !counters->total_calls)
            continue;
        counters->reported = 1U;
        printf("[app] svc slot=%lu kind=%s calls=%lu samples=%lu/%lu/%lu/%lu "
               "e2d=%lu/%lu d2h=%lu/%lu h=%lu/%lu h2e=%lu/%lu\r\n",
               (unsigned long)slot, kind ? "gl" : "api",
               (unsigned long)counters->total_calls,
               (unsigned long)counters->entry_to_dispatch.calls,
               (unsigned long)counters->dispatch_to_handler.calls,
               (unsigned long)counters->handler.calls,
               (unsigned long)counters->handler_to_eret.calls,
               (unsigned long)counter_average(&counters->entry_to_dispatch,
                                              counters->entry_to_dispatch.calls),
               (unsigned long)counters->entry_to_dispatch.max,
               (unsigned long)counter_average(&counters->dispatch_to_handler,
                                              counters->dispatch_to_handler.calls),
               (unsigned long)counters->dispatch_to_handler.max,
               (unsigned long)counter_average(&counters->handler,
                                              counters->handler.calls),
               (unsigned long)counters->handler.max,
               (unsigned long)counter_average(&counters->handler_to_eret,
                                              counters->handler_to_eret.calls),
               (unsigned long)counters->handler_to_eret.max);
    }
}
#ifdef ZZAPP_SVC_COUNTERS_TEST
void zzapp_svc_counters_test_eret(uint32_t slot, uint32_t stamp)
{
    zzapp_svc_counters_t *counters;

    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return;
    counters = &zzapp_svc_counters[slot][zzapp_svc_counter_class[slot]];
    if (counters->handler_started)
        counter_add(&counters->handler_to_eret,
                    stamp - counters->handler_done_stamp);
}

void zzapp_svc_counters_test_snapshot(uint32_t slot, uint32_t *total_calls,
                                      uint32_t samples[4], uint32_t averages[4])
{
    zzapp_svc_counters_t *counters;
    zzapp_svc_cost_t *costs;
    uint32_t i;

    if (slot >= ZZAPP_AS_SLOT_COUNT || !total_calls || !samples || !averages)
        return;
    counters = &zzapp_svc_counters[slot][0];
    costs = &counters->entry_to_dispatch;
    *total_calls = counters->total_calls;
    for (i = 0U; i < 4U; ++i) {
        samples[i] = costs[i].calls;
        averages[i] = counter_average(&costs[i], costs[i].calls);
    }
}
#endif

#else
static inline void counters_dispatch(uint32_t slot, int gl)
{ (void)slot; (void)gl; }
static inline void counters_handler_start(uint32_t slot)
{ (void)slot; }
static inline void counters_handler_end(uint32_t slot)
{ (void)slot; }
#endif
#if ZZAPP_PERF_COUNTERS
typedef struct {
    uint32_t gl_calls;
    uint32_t opcodes[GL_COUNT];
    uint32_t lock_acquisitions;
    uint32_t lock_contention;
    uint32_t descriptor_failures;
    uint32_t pointer_failures;
    uint32_t array_span_checks;
    uint32_t array_span_fallbacks;
    zzapp_svc_cost_t descriptor;
    zzapp_svc_cost_t lock;
    zzapp_svc_cost_t frontend;
    uint32_t descriptor_stamp;
    uint32_t frontend_stamp;
    uint32_t lock_stamp;
    uint32_t lock_cycles;
    uint8_t sampled;
    uint8_t frontend_active;
} zzapp_gl_frame_telemetry_t;

typedef struct {
    uint32_t frames;
    uint32_t gl_calls;
    uint32_t max_gl_calls;
    uint32_t opcodes[GL_COUNT];
    uint32_t lock_acquisitions;
    uint32_t lock_contention;
    uint32_t descriptor_failures;
    uint32_t pointer_failures;
    uint32_t array_span_checks;
    uint32_t array_span_fallbacks;
    zzapp_svc_cost_t boundary;
    zzapp_svc_cost_t descriptor;
    zzapp_svc_cost_t lock;
    zzapp_svc_cost_t frontend;
    uint64_t frame_us;
} zzapp_gl_report_telemetry_t;

typedef struct {
    zzapp_gl_frame_telemetry_t frame;
    zzapp_gl_report_telemetry_t pending;
    uint64_t last_report_us;
    uint64_t last_swap_us;
    uint32_t sample_sequence;
    uint8_t swap_started;
} zzapp_gl_telemetry_t;

static zzapp_gl_telemetry_t gl_telemetry[ZZAPP_AS_SLOT_COUNT];

static void gl_cost_add(zzapp_svc_cost_t *to, const zzapp_svc_cost_t *from)
{
    to->sum += from->sum;
    to->calls += from->calls;
    if (from->max > to->max)
        to->max = from->max;
}

static void gl_telemetry_frame_begin(uint32_t slot, uint32_t number)
{
    zzapp_gl_telemetry_t *telemetry = &gl_telemetry[slot];
    zzapp_gl_frame_telemetry_t *frame = &telemetry->frame;

    frame->gl_calls++;
    if (number < GL_COUNT)
        frame->opcodes[number]++;
    frame->sampled = (++telemetry->sample_sequence & 63U) == 0U;
    if (frame->sampled)
        frame->descriptor_stamp = pmccntr();
}

static void gl_telemetry_descriptor_end(uint32_t slot)
{
    zzapp_gl_frame_telemetry_t *frame = &gl_telemetry[slot].frame;

    if (frame->sampled)
        counter_add(&frame->descriptor, pmccntr() - frame->descriptor_stamp);
}

static void gl_telemetry_descriptor_failure(uint32_t slot)
{
    gl_telemetry[slot].frame.descriptor_failures++;
}

static void gl_telemetry_pointer_failure(uint32_t slot)
{
    gl_telemetry[slot].frame.pointer_failures++;
}

static void gl_telemetry_array_span(uint32_t slot)
{
    gl_telemetry[slot].frame.array_span_checks++;
}
static void gl_telemetry_array_fallback(uint32_t slot)
{
    gl_telemetry[slot].frame.array_span_fallbacks++;
}

static inline void gl_telemetry_frontend_begin(uint32_t slot)
{
    zzapp_gl_frame_telemetry_t *frame;

    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return;
    frame = &gl_telemetry[slot].frame;
    frame->frontend_active = 1U;
    frame->lock_cycles = 0U;
    if (frame->sampled)
        frame->frontend_stamp = pmccntr();
}

static inline void gl_telemetry_frontend_end(uint32_t slot)
{
    zzapp_gl_frame_telemetry_t *frame;

    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return;
    frame = &gl_telemetry[slot].frame;
    if (frame->sampled && frame->frontend_active) {
        uint32_t cycles = pmccntr() - frame->frontend_stamp;

        counter_add(&frame->frontend, cycles - frame->lock_cycles);
    }
    frame->frontend_active = 0U;
}

void zzapp_svc_gl_telemetry_lock_begin(uint32_t slot)
{
    zzapp_gl_frame_telemetry_t *frame;

    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return;
    frame = &gl_telemetry[slot].frame;
    if (frame->frontend_active && frame->sampled)
        frame->lock_stamp = pmccntr();
}

void zzapp_svc_gl_telemetry_lock_end(uint32_t slot, int contended)
{
    zzapp_gl_frame_telemetry_t *frame;

    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return;
    frame = &gl_telemetry[slot].frame;
    if (!frame->frontend_active)
        return;
    frame->lock_acquisitions++;
    if (contended)
        frame->lock_contention++;
    if (frame->sampled) {
        uint32_t cycles = pmccntr() - frame->lock_stamp;

        frame->lock_cycles = cycles;
        counter_add(&frame->lock, cycles);
    }
}

static uint32_t gl_cost_average(const zzapp_svc_cost_t *cost)
{
    return counter_average(cost, cost->calls);
}

static void gl_telemetry_top(const zzapp_gl_report_telemetry_t *report,
                             zzapp_gl_telemetry_snapshot_t *snapshot)
{
    uint32_t rank;

    for (rank = 0U; rank < ZZAPP_GL_TELEMETRY_TOP; ++rank) {
        uint32_t slot;
        uint32_t best_slot = UINT32_MAX;
        uint32_t best_count = 0U;

        for (slot = 0U; slot < GL_COUNT; ++slot) {
            uint32_t prior;

            for (prior = 0U; prior < rank; ++prior)
                if (snapshot->top_slots[prior] == slot)
                    break;
            if (prior != rank)
                continue;
            if (report->opcodes[slot] > best_count ||
                (report->opcodes[slot] == best_count && slot < best_slot)) {
                best_count = report->opcodes[slot];
                best_slot = slot;
            }
        }
        snapshot->top_slots[rank] = best_slot;
        snapshot->top_counts[rank] = best_count;
    }
}

static void gl_telemetry_snapshot(const zzapp_gl_report_telemetry_t *report,
                                  zzapp_gl_telemetry_snapshot_t *snapshot)
{
    uint64_t boundary;
    uint64_t descriptor;
    uint64_t lock;
    uint64_t frame_cycles;

    if (!snapshot)
        return;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->frames = report->frames;
    snapshot->gl_calls = report->gl_calls;
    snapshot->gl_calls_per_frame = report->frames ?
        report->gl_calls / report->frames : 0U;
    snapshot->max_gl_calls = report->max_gl_calls;
    snapshot->lock_acquisitions = report->frames ?
        report->lock_acquisitions / report->frames : 0U;
    snapshot->lock_contention = report->lock_contention;
    snapshot->descriptor_failures = report->descriptor_failures;
    snapshot->pointer_failures = report->pointer_failures;
    snapshot->array_span_checks = report->array_span_checks;
    snapshot->array_span_fallbacks = report->array_span_fallbacks;
    snapshot->average_boundary_cycles = gl_cost_average(&report->boundary);
    snapshot->average_descriptor_cycles = gl_cost_average(&report->descriptor);
    snapshot->average_lock_cycles = counter_average(&report->lock,
                                                     report->descriptor.calls);
    snapshot->average_frontend_cycles = counter_average(&report->frontend,
                                                         report->descriptor.calls);
    boundary = (uint64_t)snapshot->average_boundary_cycles * report->gl_calls;
    descriptor = (uint64_t)snapshot->average_descriptor_cycles * report->gl_calls;
    lock = (uint64_t)snapshot->average_lock_cycles * report->gl_calls;
    frame_cycles = report->frame_us * 666U;
    if (frame_cycles) {
        snapshot->boundary_share_percent = (uint32_t)(boundary * 100U /
                                                       frame_cycles);
        snapshot->descriptor_share_percent = (uint32_t)(descriptor * 100U /
                                                         frame_cycles);
        snapshot->lock_share_percent = (uint32_t)(lock * 100U /
                                                   frame_cycles);
    }
    gl_telemetry_top(report, snapshot);
}

static void gl_telemetry_frame_reset(zzapp_gl_frame_telemetry_t *frame,
                                     zzapp_svc_counters_t *boundary)
{
    uint32_t sample_sequence = boundary->total_calls;

    memset(frame, 0, sizeof(*frame));
    memset(boundary, 0, sizeof(*boundary));
    boundary->total_calls = sample_sequence;
}

void zzapp_svc_gl_telemetry_reset(uint32_t slot)
{
    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return;
    memset(&gl_telemetry[slot], 0, sizeof(gl_telemetry[slot]));
    memset(&zzapp_svc_counters[slot][1], 0,
           sizeof(zzapp_svc_counters[slot][1]));
}

int zzapp_svc_gl_telemetry_swap(uint32_t slot, uint64_t now_us,
                                 zzapp_gl_telemetry_snapshot_t *snapshot)
{
    zzapp_gl_telemetry_t *telemetry;
    zzapp_gl_frame_telemetry_t *frame;
    zzapp_gl_report_telemetry_t *report;
    zzapp_svc_counters_t *boundary;
    zzapp_gl_telemetry_snapshot_t local;
    zzapp_gl_telemetry_snapshot_t *out = snapshot ? snapshot : &local;
    uint32_t i;

    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return 0;
    telemetry = &gl_telemetry[slot];
    frame = &telemetry->frame;
    report = &telemetry->pending;
    boundary = &zzapp_svc_counters[slot][1];
    if (!telemetry->swap_started) {
        telemetry->last_swap_us = now_us;
        telemetry->last_report_us = now_us;
        telemetry->swap_started = 1U;
        gl_telemetry_frame_reset(frame, boundary);
        if (snapshot)
            memset(snapshot, 0, sizeof(*snapshot));
        return 0;
    }
    report->frames++;
    report->gl_calls += frame->gl_calls;
    if (frame->gl_calls > report->max_gl_calls)
        report->max_gl_calls = frame->gl_calls;
    report->lock_acquisitions += frame->lock_acquisitions;
    report->lock_contention += frame->lock_contention;
    report->descriptor_failures += frame->descriptor_failures;
    report->pointer_failures += frame->pointer_failures;
    report->array_span_checks += frame->array_span_checks;
    report->array_span_fallbacks += frame->array_span_fallbacks;
    for (i = 0U; i < GL_COUNT; ++i)
        report->opcodes[i] += frame->opcodes[i];
    report->boundary.sum += boundary->entry_to_dispatch.sum +
                            boundary->dispatch_to_handler.sum +
                            boundary->handler_to_eret.sum;
    report->boundary.calls += boundary->entry_to_dispatch.calls;
    gl_cost_add(&report->descriptor, &frame->descriptor);
    gl_cost_add(&report->lock, &frame->lock);
    gl_cost_add(&report->frontend, &frame->frontend);
    report->frame_us += now_us - telemetry->last_swap_us;
    telemetry->last_swap_us = now_us;
    gl_telemetry_frame_reset(frame, boundary);
    if (now_us - telemetry->last_report_us < 1000000ULL) {
        if (snapshot)
            gl_telemetry_snapshot(report, snapshot);
        return 0;
    }
    telemetry->last_report_us = now_us;
    gl_telemetry_snapshot(report, out);
    memset(report, 0, sizeof(*report));
    return 1;
}

void zzapp_svc_gl_telemetry_report(uint32_t slot,
                                    const zzapp_gl_telemetry_snapshot_t *snapshot)
{
#ifndef ZZAPP_HOST_TEST
    if (!snapshot)
        return;
    printf("[app] gl slot=%lu frames=%lu calls=%lu/%lu top=%lu:%lu,%lu:%lu,"
           "%lu:%lu,%lu:%lu,%lu:%lu,%lu:%lu,%lu:%lu,%lu:%lu lock=%lu/%lu "
           "fail=%lu/%lu span=%lu/%lu avg=%lu/%lu/%lu/%lu share=%lu/%lu/%lu%%\r\n",
           (unsigned long)slot, (unsigned long)snapshot->frames,
           (unsigned long)snapshot->gl_calls_per_frame,
           (unsigned long)snapshot->max_gl_calls,
           (unsigned long)snapshot->top_slots[0], (unsigned long)snapshot->top_counts[0],
           (unsigned long)snapshot->top_slots[1], (unsigned long)snapshot->top_counts[1],
           (unsigned long)snapshot->top_slots[2], (unsigned long)snapshot->top_counts[2],
           (unsigned long)snapshot->top_slots[3], (unsigned long)snapshot->top_counts[3],
           (unsigned long)snapshot->top_slots[4], (unsigned long)snapshot->top_counts[4],
           (unsigned long)snapshot->top_slots[5], (unsigned long)snapshot->top_counts[5],
           (unsigned long)snapshot->top_slots[6], (unsigned long)snapshot->top_counts[6],
           (unsigned long)snapshot->top_slots[7], (unsigned long)snapshot->top_counts[7],
           (unsigned long)snapshot->lock_acquisitions,
           (unsigned long)snapshot->lock_contention,
           (unsigned long)snapshot->descriptor_failures,
           (unsigned long)snapshot->pointer_failures,
           (unsigned long)snapshot->array_span_checks,
           (unsigned long)snapshot->array_span_fallbacks,
           (unsigned long)snapshot->average_boundary_cycles,
           (unsigned long)snapshot->average_descriptor_cycles,
           (unsigned long)snapshot->average_lock_cycles,
           (unsigned long)snapshot->average_frontend_cycles,
           (unsigned long)snapshot->boundary_share_percent,
           (unsigned long)snapshot->descriptor_share_percent,
           (unsigned long)snapshot->lock_share_percent);
#else
    (void)slot;
    (void)snapshot;
#endif
}
#else
static inline void gl_telemetry_frame_begin(uint32_t slot, uint32_t number)
{ (void)slot; (void)number; }
static inline void gl_telemetry_descriptor_end(uint32_t slot)
{ (void)slot; }
static inline void gl_telemetry_descriptor_failure(uint32_t slot)
{ (void)slot; }
static inline void gl_telemetry_pointer_failure(uint32_t slot)
{ (void)slot; }
static inline void gl_telemetry_array_span(uint32_t slot)
{ (void)slot; }
static inline void gl_telemetry_array_fallback(uint32_t slot)
{ (void)slot; }
static inline void gl_telemetry_frontend_begin(uint32_t slot)
{ (void)slot; }
static inline void gl_telemetry_frontend_end(uint32_t slot)
{ (void)slot; }
#endif
static uintptr_t gl_alias[ZZAPP_AS_SLOT_COUNT];
typedef struct {
    uint32_t alignment;
    uint32_t row_length;
    uint32_t skip_rows;
    uint32_t skip_pixels;
} gl_pixel_store_t;
typedef struct {
    uint32_t pointer;
    int32_t size;
    uint32_t type;
    int32_t stride;
    uint8_t enabled;
} gl_client_array_t;
typedef struct {
    gl_client_array_t vertex;
    gl_client_array_t color;
    gl_client_array_t texcoord[GL_TEXTURE_UNITS];
    gl_pixel_store_t pack;
    gl_pixel_store_t unpack;
    uint32_t generation;
    uint32_t active_texture;
    uint32_t error;
    uint8_t in_begin;
    uint8_t lock_held;
} gl_client_state_t;
static gl_client_state_t gl_client[ZZAPP_AS_SLOT_COUNT];


#define P(a, d, l, n, e, c) { a, d, l, n, e, c, 0U, 0U, 0U }
#define N P(0U, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_FIXED, 0U, 0U, 0U)
#define D(a, r, p0, p1, p2) { a, 0U, r, 0U, 0U, { p0, p1, p2 } }
#define DP(a, r, p0, p1, p2) { a, 0U, r, 1U, 0U, { p0, p1, p2 } }
#define DF(a, v, r, p0, p1, p2) { a, v, r, 0U, 0U, { p0, p1, p2 } }
#define DL(a, r, p0, p1, p2) { a, 0U, r, 0U, ZZAPP_SVC_FAST_NO_LOCK, \
                                { p0, p1, p2 } }
#define IN_FIXED(a, n, c) P(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_FIXED, 0U, n, c)
#define OUT_FIXED(a, n, c) P(a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_FIXED, 0U, n, c)
#define OUT_OPTIONAL(a, n, c) { a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_FIXED, 0U, n, c, 1U, 0U, 0U }
#define IO_FIXED(a, n, c) P(a, ZZAPP_SVC_POINTER_INOUT, ZZAPP_SVC_LENGTH_FIXED, 0U, n, c)
#define IN_ARG(a, n) P(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_ARGUMENT, n, 1U, 0U)
#define OUT_ARG(a, n) P(a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_ARGUMENT, n, 1U, 0U)
#define IN_COUNT(a, n, e) P(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_COUNT, n, e, 0U)
#define OUT_COUNT(a, n, e) P(a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_COUNT, n, e, 0U)
#define IN_STRING(a) P(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_STRING, 0U, 1U, 1U)
#define IN_PIXELS(a, n, f, t) { a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_GL_PIXELS, n, 1U, 0U, 0U, f, t }
#define OUT_PIXELS(a, n, f, t) { a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_GL_PIXELS, n, 1U, 0U, 0U, f, t }
#define OUT_GL_GET(a) P(a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_GL_GET, 0U, 4U, 1U)
#define IN_INDICES(a, n) P(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_GL_INDICES, n, 1U, 0U)

/* The descriptors are the ABI audit: argument words include 64-bit values and
 * stack words, and every user pointer has its direction and extent. */
static const zzapp_svc_descriptor_t api_descriptors[API_COUNT] = {
    [API_SLOT(exit)] = D(1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(yield)] = D(0, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(should_stop)] = DL(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(time_us)] = DL(0, ZZAPP_SVC_RETURN_U64, N, N, N),
    [API_SLOT(sleep_us)] = D(1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(alloc)] = D(3, ZZAPP_SVC_RETURN_POINTER, N, N, N),
    [API_SLOT(free)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 1U, 0U), N, N),
    [API_SLOT(heap_range)] = D(2, ZZAPP_SVC_RETURN_VOID, OUT_OPTIONAL(0, 4U, 1U), OUT_OPTIONAL(1, 4U, 1U), N),
    [API_SLOT(log)] = D(2, ZZAPP_SVC_RETURN_VOID, IN_ARG(0, 1), N, N),
    [API_SLOT(file_open)] = D(2, ZZAPP_SVC_RETURN_INT, IN_STRING(0), N, N),
    [API_SLOT(file_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(file_read)] = D(4, ZZAPP_SVC_RETURN_INT, OUT_ARG(1, 2), OUT_FIXED(3, 4U, 1U), N),
    [API_SLOT(file_write)] = D(4, ZZAPP_SVC_RETURN_INT, IN_ARG(1, 2), OUT_FIXED(3, 4U, 1U), N),
    [API_SLOT(file_seek)] = D(6, ZZAPP_SVC_RETURN_INT, OUT_OPTIONAL(5, 8U, 1U), N, N),
    [API_SLOT(file_stat)] = D(2, ZZAPP_SVC_RETURN_INT, IN_STRING(0), OUT_FIXED(1, sizeof(zzapp_stat), 1U), N),
    [API_SLOT(file_list_open)] = D(1, ZZAPP_SVC_RETURN_INT, IN_STRING(0), N, N),
    [API_SLOT(file_list_next)] = D(2, ZZAPP_SVC_RETURN_INT, OUT_FIXED(1, sizeof(zzapp_dirent), 1U), N, N),
    [API_SLOT(file_list_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_open)] = D(5, ZZAPP_SVC_RETURN_INT, OUT_FIXED(4, sizeof(zzapp_surface), 1U), N, N),
    [API_SLOT(display_flip)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_vsync)] = D(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(input_poll)] = D(3, ZZAPP_SVC_RETURN_INT, OUT_COUNT(0, 1, sizeof(zzapp_event)), OUT_FIXED(2, 4U, 1U), N),
    [API_SLOT(audio_open)] = D(3, ZZAPP_SVC_RETURN_INT, OUT_FIXED(2, sizeof(zzapp_audio), 1U), N, N),
    [API_SLOT(audio_close)] = D(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(gl_bind)] = D(1, ZZAPP_SVC_RETURN_POINTER, N, N, N),
    [API_SLOT(gl_swap)] = D(0, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(gl_unbind)] = D(0, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(display_flip_async)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_flip_idle)] = D(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_refresh)] = D(1, ZZAPP_SVC_RETURN_INT, IO_FIXED(0, sizeof(zzapp_surface), 1U), N, N),
    [API_SLOT(gl_bind_flags)] = D(2, ZZAPP_SVC_RETURN_POINTER, N, N, N),
    [API_SLOT(display_set_palette)] = D(4, ZZAPP_SVC_RETURN_INT, IN_COUNT(3, 2, 4U), N, N),
    [API_SLOT(display_mailbox_replaced)] = D(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(host_address)] = D(2, ZZAPP_SVC_RETURN_INT, IN_FIXED(0, 1U, 0U), OUT_FIXED(1, 4U, 1U), N),
    [API_SLOT(mem_info)] = D(1, ZZAPP_SVC_RETURN_INT, OUT_FIXED(0, sizeof(zzapp_mem_info), 1U), N, N),
    [API_SLOT(map_file)] = D(3, ZZAPP_SVC_RETURN_INT, IN_STRING(0), OUT_FIXED(1, 4U, 1U), OUT_FIXED(2, 4U, 1U)),
    [API_SLOT(unmap_file)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 1U, 0U), N, N),
    [API_SLOT(sha256)] = D(3, ZZAPP_SVC_RETURN_INT, IN_ARG(0, 1), OUT_FIXED(2, 32U, 1U), N),
    [API_SLOT(decompress)] = D(7, ZZAPP_SVC_RETURN_INT, IN_ARG(2, 3), OUT_ARG(4, 5), OUT_FIXED(6, sizeof(zzapp_service_result), 1U)),
    [API_SLOT(image_open)] = D(3, ZZAPP_SVC_RETURN_INT, IN_FIXED(0, sizeof(zzapp_image_open), 1U), OUT_FIXED(1, 4U, 1U), OUT_FIXED(2, sizeof(zzapp_image_result), 1U)),
    [API_SLOT(image_decode)] = D(5, ZZAPP_SVC_RETURN_INT, IN_ARG(1, 2), OUT_FIXED(4, sizeof(zzapp_image_result), 1U), N),
    [API_SLOT(image_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(audio_decode_open)] = D(2, ZZAPP_SVC_RETURN_INT, OUT_FIXED(1, 4U, 1U), N, N),
    [API_SLOT(audio_decode)] = D(7, ZZAPP_SVC_RETURN_INT, IN_ARG(1, 2), OUT_COUNT(4, 5, 4U), OUT_FIXED(6, sizeof(zzapp_audio_decode_result), 1U)),
    [API_SLOT(audio_decode_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(console_write)] = D(3, ZZAPP_SVC_RETURN_INT, IN_ARG(0, 1), OUT_FIXED(2, 4U, 1U), N),
    [API_SLOT(console_read)] = D(3, ZZAPP_SVC_RETURN_INT, OUT_ARG(0, 1), OUT_FIXED(2, 4U, 1U), N),
    [API_SLOT(msg_send)] = D(1, ZZAPP_SVC_RETURN_INT, IN_FIXED(0, 32U, 1U), N, N),
    [API_SLOT(msg_poll)] = DP(1, ZZAPP_SVC_RETURN_INT, OUT_FIXED(0, 32U, 1U), N, N),
    [API_SLOT(heap_grow)] = D(2, ZZAPP_SVC_RETURN_INT, OUT_FIXED(1, 4U, 1U), N, N),
    [API_SLOT(heap_shrink)] = D(2, ZZAPP_SVC_RETURN_INT, OUT_FIXED(1, 4U, 1U), N, N),
    [API_SLOT(heap_info)] = D(1, ZZAPP_SVC_RETURN_INT, OUT_FIXED(0, sizeof(zzapp_heap_info), 1U), N, N)
};

#define G0(f, r) [GL_SLOT(f)] = D(0, r, N, N, N)
#define G1(f, r) [GL_SLOT(f)] = D(1, r, N, N, N)
#define G2(f, r) [GL_SLOT(f)] = D(2, r, N, N, N)
#define G3(f, r) [GL_SLOT(f)] = D(3, r, N, N, N)
#define G4(f, r) [GL_SLOT(f)] = D(4, r, N, N, N)
#define G6(f, r) [GL_SLOT(f)] = D(6, r, N, N, N)
#define G8(f, r) [GL_SLOT(f)] = D(8, r, N, N, N)
#define G9(f, r) [GL_SLOT(f)] = D(9, r, N, N, N)
static const zzapp_svc_descriptor_t gl_descriptors[GL_COUNT] = {
    G1(Begin, ZZAPP_SVC_RETURN_VOID), G0(End, ZZAPP_SVC_RETURN_VOID),
    DF(0, 2, ZZAPP_SVC_RETURN_VOID, N, N, N), DF(0, 3, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(Vertex3fv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 12U, 1U), N, N),
    DF(0, 2, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(TexCoord2fv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 8U, 1U), N, N),
    DF(0, 3, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(Color3fv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 12U, 1U), N, N),
    [GL_SLOT(Color3ubv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 3U, 1U), N, N),
    DF(0, 4, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(Color4fv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 16U, 1U), N, N),
    G4(Color4ub, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(Color4ubv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 4U, 1U), N, N),
    G1(Enable, ZZAPP_SVC_RETURN_VOID), G1(Disable, ZZAPP_SVC_RETURN_VOID),
    G1(IsEnabled, ZZAPP_SVC_RETURN_INT), DF(1, 1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    G2(BlendFunc, ZZAPP_SVC_RETURN_VOID), G1(DepthFunc, ZZAPP_SVC_RETURN_VOID),
    G1(DepthMask, ZZAPP_SVC_RETURN_VOID), DF(0, 4, ZZAPP_SVC_RETURN_VOID, N, N, N),
    G1(CullFace, ZZAPP_SVC_RETURN_VOID), G1(FrontFace, ZZAPP_SVC_RETURN_VOID),
    G1(ShadeModel, ZZAPP_SVC_RETURN_VOID), G2(PolygonMode, ZZAPP_SVC_RETURN_VOID),
    G1(Clear, ZZAPP_SVC_RETURN_VOID), DF(0, 4, ZZAPP_SVC_RETURN_VOID, N, N, N),
    G4(Viewport, ZZAPP_SVC_RETURN_VOID), G4(Scissor, ZZAPP_SVC_RETURN_VOID),
    G1(MatrixMode, ZZAPP_SVC_RETURN_VOID), G0(LoadIdentity, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(LoadMatrixf)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 64U, 1U), N, N),
    G0(PushMatrix, ZZAPP_SVC_RETURN_VOID), G0(PopMatrix, ZZAPP_SVC_RETURN_VOID),
    DF(0, 4, ZZAPP_SVC_RETURN_VOID, N, N, N), DF(0, 3, ZZAPP_SVC_RETURN_VOID, N, N, N),
    DF(0, 3, ZZAPP_SVC_RETURN_VOID, N, N, N), DF(0, 12, ZZAPP_SVC_RETURN_VOID, N, N, N),
    DF(0, 12, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(TexImage2D)] = D(9, ZZAPP_SVC_RETURN_VOID, IN_PIXELS(8, 3, 6, 7), N, N),
    [GL_SLOT(TexSubImage2D)] = D(9, ZZAPP_SVC_RETURN_VOID, IN_PIXELS(8, 4, 6, 7), N, N),
    G3(TexParameteri, ZZAPP_SVC_RETURN_VOID), DF(2, 1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    G3(TexEnvi, ZZAPP_SVC_RETURN_VOID), DF(2, 1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(GenTextures)] = D(2, ZZAPP_SVC_RETURN_VOID, OUT_COUNT(1, 0, 4U), N, N),
    [GL_SLOT(DeleteTextures)] = D(2, ZZAPP_SVC_RETURN_VOID, IN_COUNT(1, 0, 4U), N, N),
    G2(BindTexture, ZZAPP_SVC_RETURN_VOID), G0(Finish, ZZAPP_SVC_RETURN_VOID),
    G0(Flush, ZZAPP_SVC_RETURN_VOID), G1(DrawBuffer, ZZAPP_SVC_RETURN_VOID),
    G1(ReadBuffer, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(ReadPixels)] = D(7, ZZAPP_SVC_RETURN_VOID, OUT_PIXELS(6, 2, 4, 5), N, N),
    G1(GetString, ZZAPP_SVC_RETURN_POINTER),
    [GL_SLOT(GetIntegerv)] = D(2, ZZAPP_SVC_RETURN_VOID, OUT_GL_GET(1), N, N),
    [GL_SLOT(GetFloatv)] = D(2, ZZAPP_SVC_RETURN_VOID, OUT_GL_GET(1), N, N),
    G0(GetError, ZZAPP_SVC_RETURN_INT), G2(Hint, ZZAPP_SVC_RETURN_VOID),
    G2(PixelStorei, ZZAPP_SVC_RETURN_VOID), DF(0, 8, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(Build2DMipmaps)] = D(7, ZZAPP_SVC_RETURN_INT, IN_PIXELS(6, 2, 4, 5), N, N),
    [GL_SLOT(VertexPointer)] = D(4, ZZAPP_SVC_RETURN_VOID, IN_FIXED(3, 1U, 0U), N, N),
    [GL_SLOT(TexCoordPointer)] = D(4, ZZAPP_SVC_RETURN_VOID, IN_FIXED(3, 1U, 0U), N, N),
    [GL_SLOT(ColorPointer)] = D(4, ZZAPP_SVC_RETURN_VOID, IN_FIXED(3, 1U, 0U), N, N),
    G1(EnableClientState, ZZAPP_SVC_RETURN_VOID), G1(DisableClientState, ZZAPP_SVC_RETURN_VOID),
    G1(ClientActiveTextureARB, ZZAPP_SVC_RETURN_VOID), G3(DrawArrays, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(DrawElements)] = D(4, ZZAPP_SVC_RETURN_VOID, IN_INDICES(3, 1), N, N),
    G2(LockArrays, ZZAPP_SVC_RETURN_VOID), G0(UnlockArrays, ZZAPP_SVC_RETURN_VOID)
};

uint32_t zzapp_svc_api_count(void) { return API_COUNT; }
uint32_t zzapp_svc_gl_count(void) { return GL_COUNT; }
const zzapp_svc_descriptor_t *zzapp_svc_api_descriptor(uint32_t slot)
{ return slot < API_COUNT && slot != API_SLOT(build_id) ? &api_descriptors[slot] : NULL; }
const zzapp_svc_descriptor_t *zzapp_svc_gl_descriptor(uint32_t slot)
{ return slot < GL_COUNT && slot != GL_SLOT(build_id) ? &gl_descriptors[slot] : NULL; }

uintptr_t zzapp_svc_api_veneer(uint32_t slot)
{ return slot < API_COUNT ? (uintptr_t)VENEER_PAGE + slot * VENEER_BYTES : 0U; }
uintptr_t zzapp_svc_gl_veneer(uint32_t slot)
{ return slot < GL_COUNT ? (uintptr_t)VENEER_PAGE + (ZZAPP_SVC_GL_FIRST + slot) * VENEER_BYTES : 0U; }
int zzapp_svc_set_veneer_alias(uint32_t slot, uintptr_t address)
{
    if (slot >= ZZAPP_AS_SLOT_COUNT || !address ||
        address & (VENEER_PAGE_BYTES - 1U))
        return ZZ_EINVAL;
    veneer_alias[slot] = address;
    return ZZ_OK;
}
int zzapp_svc_set_table_aliases(uint32_t slot, uintptr_t api_address,
                                uintptr_t gl_address)
{
    if (slot >= ZZAPP_AS_SLOT_COUNT || !api_address || !gl_address)
        return ZZ_EINVAL;
    api_alias[slot] = api_address;
    gl_alias[slot] = gl_address;
    return ZZ_OK;
}
int zzapp_svc_is_veneer(uintptr_t address)
{
    uintptr_t first = (uintptr_t)VENEER_PAGE;
    uintptr_t end = first + (ZZAPP_SVC_GL_FIRST + GL_COUNT) * VENEER_BYTES;
    return address >= first && address < end && (address - first) % VENEER_BYTES == 0U;
}

static void copy_build_id(char *destination, const char *source)
{
    uint32_t i;
    if (!source) { destination[0] = '\0'; return; }
    for (i = 0U; i + 1U < 128U && source[i]; ++i)
        destination[i] = source[i];
    destination[i] = '\0';
}

int zzapp_svc_copy_api(zzapp_api *destination, const zzapp_api *source)
{
    uintptr_t *entries;
    uint32_t i;
    if (!destination || !source || source->size > sizeof(*destination) || source->size < API_FIRST)
        return ZZ_EINVAL;
    memcpy(destination, source, source->size);
    entries = (uintptr_t *)(void *)((uint8_t *)destination + API_FIRST);
    for (i = 0U; i < API_COUNT; ++i)
        if (i != API_SLOT(build_id)) entries[i] = zzapp_svc_api_veneer(i);
    copy_build_id(user_api[0].build_id, source->build_id);
    destination->build_id = user_api[0].build_id;
    return ZZ_OK;
}
const zzapp_api *zzapp_svc_api_for_slot(uint32_t slot, const zzapp_api *source)
{
    struct user_api_table *table;
    uintptr_t *entries;
    uint32_t i;
    if (slot >= ZZAPP_AS_SLOT_COUNT || !source || source->size > sizeof(*source) || source->size < API_FIRST)
        return NULL;
    table = &user_api[slot];
    memcpy(&table->table, source, source->size);
    entries = (uintptr_t *)(void *)((uint8_t *)&table->table + API_FIRST);
    for (i = 0U; i < API_COUNT; ++i)
        if (i != API_SLOT(build_id))
            entries[i] = (veneer_alias[slot] ? veneer_alias[slot] :
                          (uintptr_t)VENEER_PAGE) + i * VENEER_BYTES;
    copy_build_id(table->build_id, source->build_id);
    table->table.build_id = (const char *)(uintptr_t)(api_alias[slot] ?
        api_alias[slot] + offsetof(struct user_api_table, build_id) :
        (uintptr_t)table->build_id);
    return &table->table;
}
zzapp_gl *zzapp_svc_gl_for_slot(uint32_t slot)
{ return slot < ZZAPP_AS_SLOT_COUNT ? &user_gl[slot].table : NULL; }
int zzapp_svc_copy_gl(zzapp_gl *destination, const zzapp_gl *source)
{
    uintptr_t *entries;
    uint32_t i;
    if (!destination || !source || source->size > sizeof(*destination) || source->size < GL_FIRST)
        return ZZ_EINVAL;
    memcpy(destination, source, source->size);
    entries = (uintptr_t *)(void *)((uint8_t *)destination + GL_FIRST);
    for (i = 0U; i < GL_COUNT; ++i)
        if (i != GL_SLOT(build_id)) entries[i] = zzapp_svc_gl_veneer(i);
    destination->build_id = NULL;
    return ZZ_OK;
}
static int zzapp_svc_copy_gl_for_slot(uint32_t slot, zzapp_gl *destination,
                                      const zzapp_gl *source)
{
    uintptr_t *entries;
    uint32_t i;

    if (zzapp_svc_copy_gl(destination, source) != ZZ_OK)
        return ZZ_EINVAL;
    if (!veneer_alias[slot])
        return ZZ_OK;
    entries = (uintptr_t *)(void *)((uint8_t *)destination + GL_FIRST);
    for (i = 0U; i < GL_COUNT; ++i)
        if (i != GL_SLOT(build_id))
            entries[i] = veneer_alias[slot] +
                         (ZZAPP_SVC_GL_FIRST + i) * VENEER_BYTES;
    return ZZ_OK;
}

static int section_allows(uint32_t descriptor, int write)
{
    uint32_t ap = (descriptor & L1_AP_MASK) >> 10;
    return (descriptor & L1_TYPE_MASK) == L1_SECTION && (ap == 3U || (!write && ap == 2U));
}
static int page_allows(uint32_t descriptor, int write)
{
    uint32_t ap = (descriptor & L2_AP_MASK) >> 4;
    uint32_t type = descriptor & L2_TYPE_MASK;

    return (type == L2_LARGE || (type & 2U) != 0U) &&
           (ap == 3U || (!write && ap == 2U));
}
int zzapp_svc_check_user(const zzapp_as_t *as, uint32_t address, uint32_t bytes, int write)
{
    uint64_t end;
    uint64_t page;
    if (!as || !as->active) return ZZ_EFAULT;
    if (!bytes) return ZZ_OK;
    end = (uint64_t)address + bytes;
    if (end > UINT32_MAX + 1ULL) return ZZ_EFAULT;
    for (page = address & ~(uint64_t)(PAGE_BYTES - 1U); page < end; page += PAGE_BYTES) {
        uint32_t address_page = (uint32_t)page;
        uint32_t descriptor = as->l1[address_page >> 20];
        if ((descriptor & L1_TYPE_MASK) == L1_TABLE) {
            const uint32_t *l2 = zzapp_as_l2_for(as, address_page);
            if (!l2 || !page_allows(l2[(address_page >> 12) & 0xffU], write)) return ZZ_EFAULT;
        } else if (!section_allows(descriptor, write)) return ZZ_EFAULT;
    }
    return ZZ_OK;
}
static uint32_t gl_type_bytes(uint32_t type)
{
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1U;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2U;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4U;
    case GL_DOUBLE: return 8U;
    default: return 0U;
    }
}
static void gl_client_reset(uint32_t slot)
{
    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return;
    memset(&gl_client[slot], 0, sizeof(gl_client[slot]));
    gl_client[slot].pack.alignment = 4U;
    gl_client[slot].unpack.alignment = 4U;
}
void zzapp_svc_gl_release(uint32_t slot, uint32_t generation)
{
    if (slot < ZZAPP_AS_SLOT_COUNT && gl_client[slot].generation == generation) {
        direct_gl[slot] = NULL;
        gl_client_reset(slot);
    }
}
static gl_client_array_t *gl_client_array(gl_client_state_t *state,
                                          uint32_t number)
{
    if (number == GL_SLOT(VertexPointer)) return &state->vertex;
    if (number == GL_SLOT(ColorPointer)) return &state->color;
    if (number == GL_SLOT(TexCoordPointer))
        return &state->texcoord[state->active_texture];
    return NULL;
}
static void gl_record_client_call(uint32_t slot, uint32_t number,
                                  const uint32_t args[9])
{
    gl_client_state_t *state;
    gl_client_array_t *array;

    if (slot >= ZZAPP_AS_SLOT_COUNT) return;
    state = &gl_client[slot];
    if (number == GL_SLOT(Begin)) {
        if (!state->in_begin)
            state->in_begin = 1U;
        return;
    }
    if (number == GL_SLOT(End)) {
        if (state->in_begin)
            state->in_begin = 0U;
        return;
    }
    array = gl_client_array(state, number);
    if (array) {
        int32_t size = (int32_t)args[0];
        int32_t stride = (int32_t)args[2];

        if (state->in_begin)
            return;
        if (size > 0 && size <= 4 && stride >= 0 && gl_type_bytes(args[1])) {
            array->size = size;
            array->type = args[1];
            array->stride = stride;
            array->pointer = args[3];
        }
        return;
    }
    if (number == GL_SLOT(ClientActiveTextureARB)) {
        if (state->in_begin)
            return;
        if (args[0] >= GL_TEXTURE0 && args[0] < GL_TEXTURE0 + GL_TEXTURE_UNITS)
            state->active_texture = args[0] - GL_TEXTURE0;
        return;
    }
    if (number == GL_SLOT(PixelStorei)) {
        gl_pixel_store_t *store;
        int32_t value = (int32_t)args[1];

        if (state->in_begin || value < 0)
            return;
        if (args[0] >= 0x0d00U && args[0] <= 0x0d05U)
            store = &state->pack;
        else if (args[0] >= 0x0cf0U && args[0] <= 0x0cf5U)
            store = &state->unpack;
        else
            return;
        if ((args[0] & 7U) == 5U) {
            if (value == 1 || value == 2 || value == 4 || value == 8)
                store->alignment = (uint32_t)value;
        } else if ((args[0] & 7U) == 2U)
            store->row_length = (uint32_t)value;
        else if ((args[0] & 7U) == 3U)
            store->skip_rows = (uint32_t)value;
        else if ((args[0] & 7U) == 4U)
            store->skip_pixels = (uint32_t)value;
        return;
    }
    if (number == GL_SLOT(EnableClientState) ||
        number == GL_SLOT(DisableClientState)) {
        uint8_t enabled = number == GL_SLOT(EnableClientState);

        if (state->in_begin)
            return;
        if (args[0] == GL_VERTEX_ARRAY) state->vertex.enabled = enabled;
        else if (args[0] == GL_COLOR_ARRAY) state->color.enabled = enabled;
        else if (args[0] == GL_TEXTURE_COORD_ARRAY)
            state->texcoord[state->active_texture].enabled = enabled;
    }
}
static int gl_validate_array_element(const zzapp_as_t *as,
                                     const gl_client_array_t *array,
                                     uint32_t index)
{
    uint32_t type_bytes;
    uint32_t element_bytes;
    uint64_t address;
    int32_t stride;

    if (!array->enabled) return 1;
    type_bytes = gl_type_bytes(array->type);
    stride = array->stride;
    if (array->size <= 0 || array->size > 4 || !type_bytes || stride < 0)
        return 1;
    element_bytes = (uint32_t)array->size * type_bytes;
    if (!array->pointer) return 0;
    address = (uint64_t)array->pointer +
              (uint64_t)index * (uint32_t)(stride ? stride : (int32_t)element_bytes);
    return address <= UINT32_MAX &&
           zzapp_svc_check_user(as, (uint32_t)address, element_bytes, 0) == ZZ_OK;
}
static int gl_validate_array_index(const zzapp_as_t *as,
                                   const gl_client_state_t *state,
                                   uint32_t index)
{
    uint32_t texture;

    if (!gl_validate_array_element(as, &state->vertex, index) ||
        !gl_validate_array_element(as, &state->color, index))
        return 0;
    for (texture = 0U; texture < GL_TEXTURE_UNITS; ++texture)
        if (!gl_validate_array_element(as, &state->texcoord[texture], index))
            return 0;
    return 1;
}
static int gl_validate_array_span(uint32_t slot, const zzapp_as_t *as,
                                  const gl_client_array_t *array,
                                  uint32_t min, uint32_t max,
                                  uint32_t element_count)
{
    uint32_t type_bytes;
    uint32_t element_bytes;
    uint32_t step;
    uint64_t lo;
    uint64_t hi;
    uint64_t pages;

    if (!array->enabled) return 1;
    type_bytes = gl_type_bytes(array->type);
    if (array->size <= 0 || array->size > 4 || !type_bytes ||
        array->stride < 0)
        return 1;
    element_bytes = (uint32_t)array->size * type_bytes;
    if (!array->pointer) return 0;
    step = array->stride ? (uint32_t)array->stride : element_bytes;
    lo = (uint64_t)array->pointer + (uint64_t)min * step;
    hi = (uint64_t)array->pointer + (uint64_t)max * step + element_bytes;
    if (lo > UINT32_MAX || hi > UINT32_MAX + 1ULL)
        return 0;
    pages = ((hi - 1U) >> 12) - (lo >> 12) + 1U;
    if (pages > element_count || hi - lo > UINT32_MAX)
        return 0;
    gl_telemetry_array_span(slot);
    return zzapp_svc_check_user(as, (uint32_t)lo, (uint32_t)(hi - lo), 0) ==
           ZZ_OK;
}
static int gl_validate_spans(uint32_t slot, const zzapp_as_t *as,
                             gl_client_state_t *state, uint32_t min,
                             uint32_t max, uint32_t element_count)
{
    uint32_t texture;

    if (!gl_validate_array_span(slot, as, &state->vertex, min, max,
                                element_count))
        return 0;
    if (!gl_validate_array_span(slot, as, &state->color, min, max,
                                element_count))
        return 0;
    for (texture = 0U; texture < GL_TEXTURE_UNITS; ++texture)
        if (!gl_validate_array_span(slot, as, &state->texcoord[texture],
                                    min, max, element_count))
            return 0;
    return 1;
}
static uint32_t gl_index_at(const uint8_t *indices, uint32_t index,
                            uint32_t index_bytes)
{
    uint32_t value = 0U;

    if (index_bytes == 1U)
        return indices[index];
    memcpy(&value, indices + index * index_bytes, index_bytes);
    return value;
}
static int gl_validate_draw(const zzapp_as_t *as, uint32_t slot,
                            uint32_t number, const uint32_t args[9])
{
    gl_client_state_t *state;
    int32_t count;
    uint32_t min;
    uint32_t max;
    int span_status;

    uint32_t i;
    uint32_t index_bytes = 0U;
    const uint8_t *indices = NULL;

    if (slot >= ZZAPP_AS_SLOT_COUNT) return 0;
    state = &gl_client[slot];
    count = (int32_t)(number == GL_SLOT(DrawArrays) ? args[2] : args[1]);
    if (count <= 0) return 1;
    if (number == GL_SLOT(DrawArrays)) {
        int32_t first = (int32_t)args[1];

        if (first < 0) return 1;
        if ((uint64_t)(uint32_t)first + (uint32_t)count > UINT32_MAX + 1ULL)
            return 0;
        min = (uint32_t)first;
        max = min + (uint32_t)count - 1U;
    } else {
        switch (args[2]) {
        case GL_UNSIGNED_BYTE: index_bytes = 1U; break;
        case GL_UNSIGNED_SHORT: index_bytes = 2U; break;
        case GL_UNSIGNED_INT: index_bytes = 4U; break;
        default: return 1;
        }
        indices = (const uint8_t *)(uintptr_t)args[3];
        if (!indices) return 1;
        min = gl_index_at(indices, 0U, index_bytes);
        max = min;
        for (i = 1U; i < (uint32_t)count; ++i) {
            uint32_t index = gl_index_at(indices, i, index_bytes);

            if (index < min) min = index;
            if (index > max) max = index;
        }
    }
    span_status = gl_validate_spans(slot, as, state, min, max,
                                    (uint32_t)count);
    if (span_status == 1)
        return 1;
    if (span_status < 0)
        return 0;
    gl_telemetry_array_fallback(slot);
    if (number == GL_SLOT(DrawArrays)) {
        for (i = 0U; i < (uint32_t)count; ++i)
            if (!gl_validate_array_index(as, state, min + i))
                return 0;
    } else {
        for (i = 0U; i < (uint32_t)count; ++i)
            if (!gl_validate_array_index(as, state,
                                         gl_index_at(indices, i, index_bytes)))
                return 0;
    }
    return 1;
}


static int stack_word(const zzapp_as_t *as, uint32_t stack, uint32_t index, uint32_t *value)
{
    uint32_t address = stack + index * 4U;
    if (!value || address < stack || zzapp_svc_check_user(as, address, 4U, 0) != ZZ_OK) return ZZ_EFAULT;
    *value = *(const uint32_t *)(uintptr_t)address;
    return ZZ_OK;
}
static int frame_arguments(const zzapp_as_t *as, const zzapp_svc_frame_t *frame,
                           uint32_t words, uint32_t args[9])
{
    uint32_t i;
    if (words > 9U) return ZZ_EFAULT;
    for (i = 0U; i < words && i < 4U; ++i) args[i] = frame->r[i];
    for (; i < words; ++i)
        if (stack_word(as, frame->user_sp, i - 4U, &args[i]) != ZZ_OK) return ZZ_EFAULT;
    return ZZ_OK;
}
static int gl_pixel_bytes(uint32_t slot, const uint32_t args[9],
                          const zzapp_svc_pointer_t *pointer,
                          uint32_t *bytes)
{
    const gl_pixel_store_t *store;
    uint32_t components;
    uint32_t element;
    uint32_t first = pointer->length_argument;
    uint32_t format = args[pointer->format_argument];
    uint32_t type = args[pointer->type_argument];
    uint64_t pixel_bytes;
    uint64_t source_width;
    uint64_t row_bytes;
    uint64_t length;
    int32_t width = (int32_t)args[first];
    int32_t height = (int32_t)args[first + 1U];

    if (slot >= ZZAPP_AS_SLOT_COUNT)
        return ZZ_EFAULT;
    if (width <= 0 || height <= 0) {
        *bytes = 0U;
        return ZZ_OK;
    }
    switch (format) {
    case 0x1903U: case 0x1906U: case 0x1909U: components = 1U; break;
    case 0x190aU: components = 2U; break;
    case 0x1907U: case 0x80e0U: components = 3U; break;
    case 0x1908U: case 0x80e1U: components = 4U; break;
    default: return ZZ_EFAULT;
    }
    switch (type) {
    case 0x1400U: case 0x1401U: element = 1U; break;
    case 0x1402U: case 0x1403U: element = 2U; break;
    case 0x1404U: case 0x1405U: case 0x1406U: element = 4U; break;
    case 0x8033U: case 0x8363U: components = 1U; element = 2U; break;
    case 0x8034U: case 0x8035U: case 0x8367U: components = 1U; element = 4U; break;
    default: return ZZ_EFAULT;
    }
    store = pointer->direction == ZZAPP_SVC_POINTER_IN ?
        &gl_client[slot].unpack : &gl_client[slot].pack;
    pixel_bytes = (uint64_t)components * element;
    source_width = store->row_length ? store->row_length : (uint32_t)width;
    row_bytes = source_width * pixel_bytes;
    row_bytes = (row_bytes + store->alignment - 1U) &
                ~(uint64_t)(store->alignment - 1U);
    length = (uint64_t)store->skip_rows * row_bytes +
             (uint64_t)store->skip_pixels * pixel_bytes +
             ((uint64_t)(uint32_t)height - 1U) * row_bytes +
             (uint64_t)(uint32_t)width * pixel_bytes;
    if (length > UINT32_MAX) return ZZ_EFAULT;
    *bytes = (uint32_t)length;
    return ZZ_OK;
}
static int gl_indices_bytes(const uint32_t args[9], uint32_t first,
                            uint32_t *bytes)
{
    uint32_t element;
    uint64_t length;
    int32_t count = (int32_t)args[first];

    if (count <= 0) {
        *bytes = 0U;
        return ZZ_OK;
    }
    switch (args[first + 1U]) {
    case 0x1401U: element = 1U; break;
    case 0x1403U: element = 2U; break;
    case 0x1405U: element = 4U; break;
    default: return ZZ_EFAULT;
    }
    length = (uint64_t)(uint32_t)count * element;
    if (length > UINT32_MAX) return ZZ_EFAULT;
    *bytes = (uint32_t)length;
    return ZZ_OK;
}
static int gl_get_bytes(const uint32_t args[9], uint32_t *bytes)
{
    *bytes = zzapp_gl_get_values(args[0]) * sizeof(uint32_t);
    return ZZ_OK;
}
static int pointer_bytes(uint32_t slot, const zzapp_svc_pointer_t *pointer,
                         const uint32_t args[9], uint32_t *bytes)
{
    uint64_t length;

    if (pointer->length == ZZAPP_SVC_LENGTH_FIXED) length = pointer->element_bytes;
    else if (pointer->length == ZZAPP_SVC_LENGTH_ARGUMENT) length = args[pointer->length_argument];
    else if (pointer->length == ZZAPP_SVC_LENGTH_COUNT)
        length = (uint64_t)args[pointer->length_argument] * pointer->element_bytes;
    else if (pointer->length == ZZAPP_SVC_LENGTH_GL_PIXELS)
        return gl_pixel_bytes(slot, args, pointer, bytes);
    else if (pointer->length == ZZAPP_SVC_LENGTH_GL_INDICES)
        return gl_indices_bytes(args, pointer->length_argument, bytes);
    else if (pointer->length == ZZAPP_SVC_LENGTH_GL_GET)
        return gl_get_bytes(args, bytes);
    else return ZZ_OK;
    if (length > UINT32_MAX) return ZZ_EFAULT;
    *bytes = (uint32_t)length;
    return ZZ_OK;
}
static int copy_string(const zzapp_as_t *as, uint32_t address, char destination[STRING_BYTES])
{
    uint32_t i;
    if (!address) return ZZ_EFAULT;
    for (i = 0U; i < STRING_BYTES; ++i) {
        if (zzapp_svc_check_user(as, address + i, 1U, 0) != ZZ_OK) return ZZ_EFAULT;
        destination[i] = *(const char *)(uintptr_t)(address + i);
        if (!destination[i]) return ZZ_OK;
    }
    return ZZ_EFAULT;
}
typedef uint32_t (*word_call_t)(uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static uint32_t call_words(uintptr_t address, const uint32_t args[9])
{
    word_call_t call = (word_call_t)address;
    return call(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
}
static int entry_at(const void *table, uint32_t size, uint32_t first, uint32_t slot,
                    uintptr_t *entry)
{
    uint64_t end = (uint64_t)first + ((uint64_t)slot + 1U) * sizeof(void *);
    if (!table || end > size) return ZZ_ENOSYS;
    *entry = *(const uintptr_t *)(const void *)((const uint8_t *)table + first + slot * sizeof(void *));
    return *entry ? ZZ_OK : ZZ_ENOSYS;
}

static int prepare_pointers(uint32_t slot, const zzapp_as_t *as,
                            const zzapp_svc_descriptor_t *descriptor,
                            uint32_t args[9], uint8_t copies[3][COPY_BYTES],
                            char strings[3][STRING_BYTES], uint32_t originals[3])
{
    uint32_t i;
    for (i = 0U; i < 3U; ++i) {
        const zzapp_svc_pointer_t *pointer = &descriptor->pointers[i];
        uint32_t bytes = 0U;
        int write;
        if (!pointer->element_bytes) continue;
        originals[i] = args[pointer->argument];
        if (pointer->optional && !originals[i])
            continue;
        write = pointer->direction != ZZAPP_SVC_POINTER_IN;
        if (pointer->length == ZZAPP_SVC_LENGTH_STRING) {
            if (copy_string(as, args[pointer->argument], strings[i]) != ZZ_OK) return ZZ_EFAULT;
            args[pointer->argument] = (uint32_t)(uintptr_t)strings[i];
            continue;
        }
        if (pointer_bytes(slot, pointer, args, &bytes) != ZZ_OK ||
            (!args[pointer->argument] && bytes) ||
            zzapp_svc_check_user(as, args[pointer->argument], bytes, write) != ZZ_OK) return ZZ_EFAULT;
        if (pointer->copy) {
            if (bytes > COPY_BYTES) return ZZ_EFAULT;
            memset(copies[i], 0, bytes);
            if (pointer->direction != ZZAPP_SVC_POINTER_OUT)
                memcpy(copies[i], (const void *)(uintptr_t)args[pointer->argument], bytes);
            args[pointer->argument] = (uint32_t)(uintptr_t)copies[i];
        }
    }
    return ZZ_OK;
}
static void finish_pointers(uint32_t slot, const zzapp_svc_descriptor_t *descriptor,
                            const uint32_t args[9],
                            uint8_t copies[3][COPY_BYTES],
                            const uint32_t originals[3], uint32_t result)
{
    uint32_t i;

    if (descriptor->result == ZZAPP_SVC_RETURN_INT && result != (uint32_t)ZZ_OK &&
        (!descriptor->copy_out_on_positive || (int32_t)result <= 0))
        return;
    for (i = 0U; i < 3U; ++i) {
        const zzapp_svc_pointer_t *pointer = &descriptor->pointers[i];
        uint32_t bytes = 0U;
        if (!pointer->element_bytes || !pointer->copy ||
            (pointer->optional && !originals[i]) ||
            pointer->direction == ZZAPP_SVC_POINTER_IN ||
            pointer_bytes(slot, pointer, args, &bytes) != ZZ_OK) continue;
        memcpy((void *)(uintptr_t)originals[i], copies[i], bytes);
    }
}

static int heap_shrink_out_overlaps(const zzapp_proc_t *proc,
                                    const uint32_t args[9],
                                    const uint32_t originals[3])
{
    uint64_t start;
    uint64_t end;
    uint64_t out;

    if (!proc || args[0] == 0U || proc->heap_break < proc->heap_base ||
        args[0] > proc->heap_break - proc->heap_base)
        return 0;
    start = (uint64_t)ZZAPP_AS_WINDOW_BASE + proc->heap_break - args[0];
    end = start + args[0];
    out = originals[0];
    return out < end && out + sizeof(void *) > start;
}

static uint32_t float_word(const zzapp_svc_frame_t *frame, uint32_t index)
{ return frame->vfp[index]; }
static float float_argument(const zzapp_svc_frame_t *frame, uint32_t index)
{ float value; uint32_t word = float_word(frame, index); memcpy(&value, &word, sizeof(value)); return value; }
static double double_argument(const zzapp_svc_frame_t *frame, uint32_t index)
{ double value; uint32_t words[2] = { float_word(frame, index * 2U), float_word(frame, index * 2U + 1U) }; memcpy(&value, words, sizeof(value)); return value; }
static int call_gl_float(const zzapp_gl *gl, uint32_t slot, const zzapp_svc_frame_t *frame,
                         const uint32_t args[9], uint32_t *result)
{
    float a = float_argument(frame, 0U), b = float_argument(frame, 1U);
    float c = float_argument(frame, 2U), d = float_argument(frame, 3U);
    if (slot == GL_SLOT(Vertex2f)) gl->Vertex2f(a, b);
    else if (slot == GL_SLOT(Vertex3f)) gl->Vertex3f(a, b, c);
    else if (slot == GL_SLOT(TexCoord2f)) gl->TexCoord2f(a, b);
    else if (slot == GL_SLOT(Color3f)) gl->Color3f(a, b, c);
    else if (slot == GL_SLOT(Color4f)) gl->Color4f(a, b, c, d);
    else if (slot == GL_SLOT(AlphaFunc)) gl->AlphaFunc(args[0], a);
    else if (slot == GL_SLOT(DepthRange)) gl->DepthRange(double_argument(frame, 0U), double_argument(frame, 1U));
    else if (slot == GL_SLOT(ClearColor)) gl->ClearColor(a, b, c, d);
    else if (slot == GL_SLOT(Rotatef)) gl->Rotatef(a, b, c, d);
    else if (slot == GL_SLOT(Translatef)) gl->Translatef(a, b, c);
    else if (slot == GL_SLOT(Scalef)) gl->Scalef(a, b, c);
    else if (slot == GL_SLOT(Ortho)) gl->Ortho(double_argument(frame, 0U), double_argument(frame, 1U), double_argument(frame, 2U), double_argument(frame, 3U), double_argument(frame, 4U), double_argument(frame, 5U));
    else if (slot == GL_SLOT(Frustum)) gl->Frustum(double_argument(frame, 0U), double_argument(frame, 1U), double_argument(frame, 2U), double_argument(frame, 3U), double_argument(frame, 4U), double_argument(frame, 5U));
    else if (slot == GL_SLOT(TexParameterf)) gl->TexParameterf(args[0], args[1], a);
    else if (slot == GL_SLOT(TexEnvf)) gl->TexEnvf(args[0], args[1], a);
    else if (slot == GL_SLOT(Perspective)) gl->Perspective(double_argument(frame, 0U), double_argument(frame, 1U), double_argument(frame, 2U), double_argument(frame, 3U));
    else return 0;
    *result = 0U;
    return 1;
}

static int dispatch_bind(const zzapp_api *api, uint32_t slot,
                         const uint32_t args[9], int flags, uint32_t *result)
{
    const zzapp_gl *gl = flags ? api->gl_bind_flags(args[0], args[1]) :
                                 api->gl_bind(args[0]);
    zzapp_proc_t *proc = zzapp_proc_slot(slot);

    if (!gl || !proc || slot >= ZZAPP_AS_SLOT_COUNT ||
        zzapp_svc_copy_gl_for_slot(slot, &user_gl[slot].table, gl) != ZZ_OK) {
        if (slot < ZZAPP_AS_SLOT_COUNT) {
            direct_gl[slot] = NULL;
            gl_client_reset(slot);
        }
        *result = 0U;
        return ZZ_OK;
    }
    gl_client_reset(slot);
    gl_client[slot].generation = proc->generation;
    direct_gl[slot] = gl;
    copy_build_id(user_gl[slot].build_id, gl->build_id);
    user_gl[slot].table.build_id = (const char *)(uintptr_t)(gl_alias[slot] ?
        gl_alias[slot] + offsetof(struct user_gl_table, build_id) :
        (uintptr_t)user_gl[slot].build_id);
    *result = (uint32_t)(gl_alias[slot] ? gl_alias[slot] :
                         (uintptr_t)&user_gl[slot].table);
    return ZZ_OK;
}
static int dispatch_api(const zzapp_api *api, uint32_t slot, zzapp_svc_frame_t *frame,
                        const zzapp_svc_descriptor_t *descriptor, uint32_t number,
                        uint32_t args[9], uint32_t *result)
{
    (void)descriptor;
    uintptr_t entry;
    if (number == API_SLOT(gl_bind)) return dispatch_bind(api, slot, args, 0, result);
    if (number == API_SLOT(gl_bind_flags)) return dispatch_bind(api, slot, args, 1, result);
    if (number == API_SLOT(time_us)) {
        uint64_t value = api->time_us();

        *result = (uint32_t)value;
        frame->r[1] = (uint32_t)(value >> 32);
        return ZZ_OK;
    }
    if (entry_at(api, api->size, API_FIRST, number, &entry) != ZZ_OK)
        return ZZ_ENOSYS;
    *result = call_words(entry, args);
    if (number == API_SLOT(gl_unbind) && slot < ZZAPP_AS_SLOT_COUNT)
        zzapp_svc_gl_release(slot, gl_client[slot].generation);
    return ZZ_OK;
}
static int dispatch_gl(uint32_t slot, const zzapp_svc_frame_t *frame,
                       uint32_t number, const uint32_t args[9], uint32_t *result)
{
    zzapp_proc_t *proc = zzapp_proc_slot(slot);
    const zzapp_gl *gl = slot < ZZAPP_AS_SLOT_COUNT ? direct_gl[slot] : NULL;
    uintptr_t entry;
    int status = ZZ_OK;

    if (!gl || !proc || gl_client[slot].generation != proc->generation) {
        status = ZZ_ENOSYS;
        goto done;
    }
    if (number == GL_SLOT(GetError) && gl_client[slot].error) {
        *result = gl_client[slot].error;
        gl_client[slot].error = 0U;
        goto done;
    }
    if (number == GL_SLOT(LockArrays) || number == GL_SLOT(UnlockArrays)) {
        gl_client_state_t *state = &gl_client[slot];
        int has_is_enabled = gl->size >=
            offsetof(zzapp_gl, IsEnabled) + sizeof(gl->IsEnabled) &&
            gl->IsEnabled;

        if (state->in_begin) {
            if (!state->error)
                state->error = GL_INVALID_OPERATION;
        } else if (number == GL_SLOT(LockArrays)) {
            if ((int32_t)args[1] < 0 || args[0] > INT32_MAX) {
                if (!state->error)
                    state->error = GL_INVALID_VALUE;
            } else if (has_is_enabled &&
                       (!gl->IsEnabled(GL_VERTEX_ARRAY) ||
                        !gl->IsEnabled(MGL_ARRAY_TRANSFORMATIONS))) {
                if (!state->error)
                    state->error = GL_INVALID_OPERATION;
            } else {
                /*
                 * The lock is intentionally ignored; do not emulate
                 * MiniGL's allocation-dependent GL_OUT_OF_MEMORY result.
                 */
                state->lock_held = 1U;
            }
        } else if (!state->lock_held) {
            if (!state->error)
                state->error = GL_INVALID_OPERATION;
        } else {
            state->lock_held = 0U;
        }
        *result = 0U;
        goto done;
    }
    if (number == GL_SLOT(GetString)) {
        const uint8_t *string = gl->GetString(args[0]);

        if (!string) {
            *result = 0U;
            goto done;
        }
        copy_build_id(user_gl[slot].build_id, (const char *)string);
        *result = (uint32_t)(gl_alias[slot] ?
            gl_alias[slot] + offsetof(struct user_gl_table, build_id) :
            (uintptr_t)user_gl[slot].build_id);
        goto done;
    }
    if (call_gl_float(gl, number, frame, args, result))
        goto done;
    if (entry_at(gl, gl->size, GL_FIRST, number, &entry) != ZZ_OK) {
        status = ZZ_ENOSYS;
        goto done;
    }
    *result = call_words(entry, args);
    gl_record_client_call(slot, number, args);
done:
    gl_telemetry_frontend_end(slot);
    return status;
}

/* Veneer number of each slot's latest SVC (GL calls are 128 and up), for
 * the stop report on the UART. */
volatile uint32_t zzapp_svc_last_number[ZZAPP_AS_SLOT_COUNT];

static void zzapp_svc_dispatch_slow(const zzapp_api *api, zzapp_proc_t *proc,
                                    uint32_t slot, uint32_t number,
                                    zzapp_svc_frame_t *frame)
{
    const zzapp_svc_descriptor_t *descriptor;
    uint32_t result = 0U;
    uint32_t args[9] = { 0U };
    uint8_t copies[3][COPY_BYTES];
    char strings[3][STRING_BYTES];
    uint32_t originals[3] = { 0U };
    int gl;
    int status;

    gl = number >= ZZAPP_SVC_GL_FIRST;
    number -= gl ? ZZAPP_SVC_GL_FIRST : 0U;
    if (gl)
        gl_telemetry_frame_begin(slot, number);
    descriptor = gl ? zzapp_svc_gl_descriptor(number) :
                      zzapp_svc_api_descriptor(number);
    if (!descriptor) {
        if (gl) {
            gl_telemetry_descriptor_failure(slot);
            gl_telemetry_descriptor_end(slot);
        }
        frame->r[0] = (uint32_t)ZZ_ENOSYS;
        return;
    }
    if (gl)
        counters_handler_start(slot);
    if (frame_arguments(&proc->as, frame, descriptor->arguments, args) != ZZ_OK ||
        prepare_pointers(slot, &proc->as, descriptor, args, copies, strings,
                         originals) != ZZ_OK) {
        if (gl) {
            gl_telemetry_pointer_failure(slot);
            gl_telemetry_descriptor_end(slot);
            counters_handler_end(slot);
        }
        frame->r[0] = (uint32_t)ZZ_EFAULT;
        return;
    }
    if (gl && (number == GL_SLOT(DrawArrays) ||
               number == GL_SLOT(DrawElements)) &&
        !gl_validate_draw(&proc->as, slot, number, args)) {
        if (!gl_client[slot].error)
            gl_client[slot].error = GL_INVALID_VALUE;
        gl_telemetry_pointer_failure(slot);
        gl_telemetry_descriptor_end(slot);
        counters_handler_end(slot);
        frame->r[0] = (uint32_t)ZZ_EFAULT;
        return;
    }
    if (!gl && number == API_SLOT(heap_shrink) &&
        heap_shrink_out_overlaps(proc, args, originals)) {
        frame->r[0] = (uint32_t)ZZ_EFAULT;
        return;
    }
    if (!gl && number == API_SLOT(image_open)) {
        const zzapp_image_open *open =
            (const zzapp_image_open *)(const void *)copies[0];

        if (zzapp_svc_check_user(&proc->as, (uint32_t)(uintptr_t)open->tile,
                                 open->tile_bytes, 1) != ZZ_OK) {
            frame->r[0] = (uint32_t)ZZ_EFAULT;
            return;
        }
    }
    if (gl) {
        gl_telemetry_descriptor_end(slot);
        gl_telemetry_frontend_begin(slot);
    } else
        counters_handler_start(slot);
    status = gl ? dispatch_gl(slot, frame, number, args, &result) :
                  dispatch_api(api, slot, frame, descriptor, number, args,
                               &result);
    counters_handler_end(slot);
    if (status != ZZ_OK) {
        frame->r[0] = (uint32_t)(status == ZZ_EFAULT ? ZZ_EFAULT : ZZ_ENOSYS);
        return;
    }
    finish_pointers(slot, descriptor, args, copies, originals, result);
    if (descriptor->result != ZZAPP_SVC_RETURN_VOID)
        frame->r[0] = result;
}

static int dispatch_fast_no_lock(const zzapp_api *api, uint32_t number,
                                 zzapp_svc_frame_t *frame)
{
    if (number == API_SLOT(should_stop)) {
        if (!api->should_stop)
            return ZZ_ENOSYS;
        frame->r[0] = (uint32_t)api->should_stop();
        return ZZ_OK;
    }
    if (number == API_SLOT(time_us)) {
        uint64_t value;

        if (!api->time_us)
            return ZZ_ENOSYS;
        value = api->time_us();
        frame->r[0] = (uint32_t)value;
        frame->r[1] = (uint32_t)(value >> 32);
        return ZZ_OK;
    }
    return ZZ_ENOSYS;
}

#if ZZAPP_SVC_PROBES && ZZAPP_PERF_COUNTERS
static void probe_invalidate_counter_tail(void)
{
    int thread = zzapp_kernel_current_thread();
    uint32_t slot = thread >= 0 && thread < (int)ZZAPP_AS_SLOT_COUNT ?
                    (uint32_t)thread : zzapp_services_current_slot();

    if (slot >= ZZAPP_AS_SLOT_COUNT)
        slot = 0U;
    zzapp_svc_counters[slot][zzapp_svc_counter_class[slot]].handler_started =
        0U;
}
#endif

#if !ZZAPP_SVC_PROBES
void zzapp_svc_dispatch(zzapp_svc_frame_t *frame)
{
    const zzapp_api *api;
    zzapp_proc_t *proc;
    const zzapp_svc_descriptor_t *descriptor;
    uint32_t number;
    uint32_t slot;
    int status;
    int thread;

    if (!frame)
        return;
    number = frame->r[12];
    if (number < API_COUNT) {
        descriptor = &api_descriptors[number];
        if (descriptor->flags & ZZAPP_SVC_FAST_NO_LOCK) {
            thread = zzapp_kernel_current_thread();
            slot = thread >= 0 && thread < (int)ZZAPP_AS_SLOT_COUNT ?
                   (uint32_t)thread : zzapp_services_current_slot();
            if (slot >= ZZAPP_AS_SLOT_COUNT)
                slot = 0U;
            counters_dispatch(slot, 0);
            api = zzapp_api_table();
            if (!api) {
                frame->r[0] = (uint32_t)ZZ_EFAULT;
                return;
            }
            zzapp_svc_last_number[slot] = number;
            counters_handler_start(slot);
            status = dispatch_fast_no_lock(api, number, frame);
            counters_handler_end(slot);
            if (status != ZZ_OK)
                frame->r[0] = (uint32_t)ZZ_ENOSYS;
            return;
        }
    }
    slot = zzapp_services_current_slot();
    if (slot >= ZZAPP_AS_SLOT_COUNT)
        slot = 0U;
    counters_dispatch(slot, number >= ZZAPP_SVC_GL_FIRST);
    api = zzapp_api_table();
    proc = zzapp_proc_slot(slot);
    if (!proc || !api) {
        frame->r[0] = (uint32_t)ZZ_EFAULT;
        return;
    }
    zzapp_svc_last_number[slot] = number;
    zzapp_svc_dispatch_slow(api, proc, slot, number, frame);
}
#else
void zzapp_svc_dispatch(zzapp_svc_frame_t *frame)
{
    const zzapp_api *api;
    const zzapp_svc_descriptor_t *descriptor;
    uint32_t number;
    uint32_t slot;
    int status;
    int thread;
    int fast = 0;

    if (!frame)
        return;
    number = frame->r[12];
    if (number == ZZAPP_SVC_PROBE_P2 || number == ZZAPP_SVC_PROBE_P3) {
#if ZZAPP_PERF_COUNTERS
        probe_invalidate_counter_tail();
#endif
        frame->r[0] = 0U;
        return;
    }
    if (number == ZZAPP_SVC_PROBE_P4)
        fast = 1;
    if (number < API_COUNT) {
        descriptor = &api_descriptors[number];
        if (descriptor->flags & ZZAPP_SVC_FAST_NO_LOCK)
            fast = 1;
    }
    if (!fast) {
        zzapp_proc_t *proc;

        slot = zzapp_services_current_slot();
        if (slot >= ZZAPP_AS_SLOT_COUNT)
            slot = 0U;
        counters_dispatch(slot, number >= ZZAPP_SVC_GL_FIRST);
        api = zzapp_api_table();
        proc = zzapp_proc_slot(slot);
        if (!proc || !api) {
            frame->r[0] = (uint32_t)ZZ_EFAULT;
            return;
        }
        zzapp_svc_last_number[slot] = number;
        zzapp_svc_dispatch_slow(api, proc, slot, number, frame);
        return;
    }
    thread = zzapp_kernel_current_thread();
    slot = thread >= 0 && thread < (int)ZZAPP_AS_SLOT_COUNT ?
           (uint32_t)thread : zzapp_services_current_slot();
    if (slot >= ZZAPP_AS_SLOT_COUNT)
        slot = 0U;
    counters_dispatch(slot, 0);
    api = zzapp_api_table();
    if (!api) {
        frame->r[0] = (uint32_t)ZZ_EFAULT;
        return;
    }
    zzapp_svc_last_number[slot] = number;
    counters_handler_start(slot);
    if (number == ZZAPP_SVC_PROBE_P4) {
        frame->r[0] = 0U;
        status = ZZ_OK;
    } else
        status = dispatch_fast_no_lock(api, number, frame);
    counters_handler_end(slot);
    if (status != ZZ_OK)
        frame->r[0] = (uint32_t)ZZ_ENOSYS;
}
#endif
