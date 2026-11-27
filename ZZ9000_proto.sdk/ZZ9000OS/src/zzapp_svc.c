/*
 * App SVC veneer tables and checked dispatch.
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "zzapp_gl_contract.h"
#include "zzapp_rec.h"
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
#define COPY_BYTES ((sizeof(aar_dirent) + 7U) & ~(size_t)7U)
#define API_FIRST offsetof(aar_api, exit)
#define GL_FIRST offsetof(aar_gl, Begin)
#define API_SLOT(field) ((uint32_t)((offsetof(aar_api, field) - API_FIRST) / sizeof(void *)))
#define GL_SLOT(field) ((uint32_t)((offsetof(aar_gl, field) - GL_FIRST) / sizeof(void *)))
#define API_COUNT ((uint32_t)((sizeof(aar_api) - API_FIRST) / sizeof(void *)))
#define GL_COUNT ((uint32_t)((sizeof(aar_gl) - GL_FIRST) / sizeof(void *)))
/* One more veneer than the table has slots: aar_ext_zz9000.gl_bind_flags. */
#define API_VENEERS (API_COUNT + 1U)
_Static_assert(API_COUNT == AAR_API_SLOT_COUNT, "aar_api slot count");
_Static_assert(GL_COUNT == AAR_GL_SLOT_COUNT, "aar_gl slot count");
_Static_assert(ZZAPP_SVC_EXT_GL_BIND_FLAGS == API_COUNT, "ext veneer number");
_Static_assert(API_VENEERS <= ZZAPP_SVC_GL_FIRST, "api veneers below gl");
/* Words a call can occupy: AAPCS argument registers and stack words. */
#define SVC_MAX_WORDS 12U
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
#define GL_NORMAL_ARRAY 0x8075U
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
__attribute__((weak)) zzapp_proc_t *zzapp_proc_slot(uint32_t pid)
{ (void)pid; return NULL; }
__attribute__((weak)) const aar_api *zzapp_api_table(void) { return NULL; }
#endif

#define GL_STRING_FIRST 0x1f00U /* GL_VENDOR through GL_EXTENSIONS */
#define GL_STRING_COUNT 4U

struct user_api_table {
    aar_api table;
    aar_ext_zz9000 ext;
    char build_id[128];
} __attribute__((aligned(VENEER_PAGE_BYTES)));
struct user_gl_table {
    aar_gl table;
    /* One app-readable string per name; later queries must not change it. */
    char strings[GL_STRING_COUNT][128];
} __attribute__((aligned(VENEER_PAGE_BYTES)));

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
/*
 * What the SVC vector counts into: the head of the record's svc part.
 * zzapp_svc_counter_block (zzapp_kernel.h) points here for the running app.
 * The assembly reads entry_stamp at offset 0, counter_class at 4 and the
 * two counter sets (api, gl) from offset 8, 88 bytes each.
 */
typedef struct {
    volatile uint32_t entry_stamp;
    volatile uint32_t counter_class;
    zzapp_svc_counters_t counters[2];
} zzapp_svc_counter_block_t;
_Static_assert(offsetof(zzapp_svc_counter_block_t, counter_class) == 4U,
               "svc block class offset");
_Static_assert(offsetof(zzapp_svc_counter_block_t, counters) == 8U,
               "svc block counters offset");
_Static_assert(sizeof(zzapp_svc_counter_block_t) ==
               ZZAPP_SVC_COUNTER_BLOCK_BYTES, "svc block size");

typedef struct {
    uint32_t gl_calls;
    /* Calls per opcode within one frame; saturates, a frame never makes
     * 65535 calls of one opcode in practice. */
    uint16_t opcodes[GL_COUNT];
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
#endif

typedef struct {
    uint32_t alignment;
    uint32_t row_length;
    uint32_t skip_rows;
    uint32_t skip_pixels;
} gl_pixel_store_t;
typedef struct {
    uint32_t pointer;
    uint32_t type;
    int32_t stride;
    uint8_t size; /* one to four components, or zero before a valid pointer */
    uint8_t enabled;
} gl_client_array_t;
typedef struct {
    gl_client_array_t vertex;
    gl_client_array_t color;
    gl_client_array_t normal;
    gl_client_array_t texcoord[GL_TEXTURE_UNITS];
    gl_pixel_store_t pack;
    gl_pixel_store_t unpack;
    uint32_t generation;
    uint32_t active_texture;
    uint32_t error;
    uint8_t in_begin;
    uint8_t lock_held;
    uint8_t strings_cached;
} gl_client_state_t;

/*
 * Everything this file keeps per app, overlaid on the svc part of its
 * process record (zzapp_rec_t.svc). The counter block is first because the
 * SVC vector finds it through the record, not through this file.
 */
typedef struct {
#if ZZAPP_PERF_COUNTERS
    zzapp_svc_counter_block_t block;
#endif
    /* Veneer number of the app's latest SVC, for the stop report. */
    uint32_t last_number;
    /* Where the app sees the veneer page and its two table pages; 0 until
     * the host maps them. User virtual addresses are 32 bits. */
    uint32_t veneer_alias;
    uint32_t api_alias;
    uint32_t gl_alias;
    const aar_gl *direct_gl;
    gl_client_state_t gl_client;
#if ZZAPP_PERF_COUNTERS
    zzapp_gl_telemetry_t telemetry;
#endif
} svc_state_t;
_Static_assert(sizeof(svc_state_t) <= ZZAPP_REC_SVC_BYTES,
               "svc state fits the record's svc part");
_Static_assert(_Alignof(svc_state_t) <= 8U, "svc part alignment");
_Static_assert(sizeof(struct user_api_table) <= ZZAPP_REC_GL_PAGE_OFFSET,
               "api table is one page");
_Static_assert(sizeof(struct user_gl_table) <= ZZAPP_REC_GL_PAGE_OFFSET,
               "gl table is one page");
#if ZZAPP_PERF_COUNTERS
_Static_assert(offsetof(svc_state_t, block) == 0U,
               "the counter block heads the svc part");
#endif

/* NULL for a pid outside the record table. */
static svc_state_t *svc_state(uint32_t pid)
{
    zzapp_rec_t *rec = zzapp_rec_by_pid(pid);

    return rec ? (svc_state_t *)(void *)rec->svc.bytes : NULL;
}

/* The app's two table pages sit in the record's 8 KiB tables area. */
static struct user_api_table *user_api_of(uint32_t pid)
{
    uint8_t *tables = zzapp_rec_tables(pid);

    return tables ? (struct user_api_table *)(void *)
        (tables + ZZAPP_REC_API_PAGE_OFFSET) : NULL;
}
static struct user_gl_table *user_gl_of(uint32_t pid)
{
    uint8_t *tables = zzapp_rec_tables(pid);

    return tables ? (struct user_gl_table *)(void *)
        (tables + ZZAPP_REC_GL_PAGE_OFFSET) : NULL;
}

uint32_t zzapp_svc_last_number_for(uint32_t pid)
{
    const svc_state_t *st = svc_state(pid);

    return st ? st->last_number : 0U;
}

#if ZZAPP_PERF_COUNTERS
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

static inline zzapp_svc_counters_t *counters_selected(svc_state_t *st)
{
    return &st->block.counters[st->block.counter_class != 0U];
}

static inline void counters_dispatch(svc_state_t *st, int gl)
{
    zzapp_svc_counters_t *counters = &st->block.counters[gl != 0];
    uint32_t stamp;

    st->block.counter_class = gl != 0;
    counters->total_calls++;
    counters->handler_started = 0U;
    if ((counters->total_calls & 63U) != 0U)
        return;
    stamp = pmccntr();
    counter_add(&counters->entry_to_dispatch, stamp - st->block.entry_stamp);
    counters->dispatch_stamp = stamp;
}

int zzapp_svc_counters_sampled(uint32_t pid)
{
    svc_state_t *st = svc_state(pid);

    if (!st)
        return 0;
    return (counters_selected(st)->total_calls & 63U) == 0U;
}

static inline void counters_handler_start(svc_state_t *st)
{
    zzapp_svc_counters_t *counters = counters_selected(st);
    uint32_t stamp;

    if ((counters->total_calls & 63U) != 0U)
        return;
    stamp = pmccntr();
    counter_add(&counters->dispatch_to_handler,
                stamp - counters->dispatch_stamp);
    counters->handler_stamp = stamp;
    counters->handler_started = 1U;
}
static inline void counters_handler_end(svc_state_t *st)
{
    zzapp_svc_counters_t *counters = counters_selected(st);
    uint32_t stamp;

    if ((counters->total_calls & 63U) != 0U)
        return;
    stamp = pmccntr();
    counter_add(&counters->handler, stamp - counters->handler_stamp);
    counters->handler_done_stamp = stamp;
}

void zzapp_svc_counters_reset(uint32_t pid)
{
    svc_state_t *st = svc_state(pid);

    if (!st)
        return;
    memset(st->block.counters, 0, sizeof(st->block.counters));
    st->block.counter_class = 0U;
    zzapp_svc_gl_telemetry_reset(pid);
}


static uint32_t counter_average(const zzapp_svc_cost_t *cost, uint32_t samples)
{
    return samples ? (uint32_t)(cost->sum / samples) : 0U;
}

void zzapp_svc_counters_report(uint32_t pid)
{
    svc_state_t *st = svc_state(pid);
    uint32_t kind;

    if (!st)
        return;
    for (kind = 0U; kind < 2U; ++kind) {
        zzapp_svc_counters_t *counters = &st->block.counters[kind];

        if (counters->reported || !counters->total_calls)
            continue;
        counters->reported = 1U;
        printf("[app] svc slot=%lu kind=%s calls=%lu samples=%lu/%lu/%lu/%lu "
               "e2d=%lu/%lu d2h=%lu/%lu h=%lu/%lu h2e=%lu/%lu\r\n",
               (unsigned long)pid, kind ? "gl" : "api",
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
void zzapp_svc_counters_test_eret(uint32_t pid, uint32_t stamp)
{
    svc_state_t *st = svc_state(pid);
    zzapp_svc_counters_t *counters;

    if (!st)
        return;
    counters = counters_selected(st);
    if (counters->handler_started)
        counter_add(&counters->handler_to_eret,
                    stamp - counters->handler_done_stamp);
}

void zzapp_svc_counters_test_snapshot(uint32_t pid, uint32_t *total_calls,
                                      uint32_t samples[4], uint32_t averages[4])
{
    svc_state_t *st = svc_state(pid);
    zzapp_svc_counters_t *counters;
    zzapp_svc_cost_t *costs;
    uint32_t i;

    if (!st || !total_calls || !samples || !averages)
        return;
    counters = &st->block.counters[0];
    costs = &counters->entry_to_dispatch;
    *total_calls = counters->total_calls;
    for (i = 0U; i < 4U; ++i) {
        samples[i] = costs[i].calls;
        averages[i] = counter_average(&costs[i], costs[i].calls);
    }
}
#endif

#else
static inline void counters_dispatch(svc_state_t *st, int gl)
{ (void)st; (void)gl; }
static inline void counters_handler_start(svc_state_t *st)
{ (void)st; }
static inline void counters_handler_end(svc_state_t *st)
{ (void)st; }
#endif
#if ZZAPP_PERF_COUNTERS

static void gl_cost_add(zzapp_svc_cost_t *to, const zzapp_svc_cost_t *from)
{
    to->sum += from->sum;
    to->calls += from->calls;
    if (from->max > to->max)
        to->max = from->max;
}

static void gl_telemetry_frame_begin(svc_state_t *st, uint32_t number)
{
    zzapp_gl_telemetry_t *telemetry = &st->telemetry;
    zzapp_gl_frame_telemetry_t *frame = &telemetry->frame;

    frame->gl_calls++;
    if (number < GL_COUNT && frame->opcodes[number] != UINT16_MAX)
        frame->opcodes[number]++;
    frame->sampled = (++telemetry->sample_sequence & 63U) == 0U;
    if (frame->sampled)
        frame->descriptor_stamp = pmccntr();
}

static void gl_telemetry_descriptor_end(svc_state_t *st)
{
    zzapp_gl_frame_telemetry_t *frame = &st->telemetry.frame;

    if (frame->sampled)
        counter_add(&frame->descriptor, pmccntr() - frame->descriptor_stamp);
}

static void gl_telemetry_descriptor_failure(svc_state_t *st)
{
    st->telemetry.frame.descriptor_failures++;
}

static void gl_telemetry_pointer_failure(svc_state_t *st)
{
    st->telemetry.frame.pointer_failures++;
}

static void gl_telemetry_array_span(svc_state_t *st)
{
    st->telemetry.frame.array_span_checks++;
}
static void gl_telemetry_array_fallback(svc_state_t *st)
{
    st->telemetry.frame.array_span_fallbacks++;
}

static inline void gl_telemetry_frontend_begin(svc_state_t *st)
{
    zzapp_gl_frame_telemetry_t *frame = &st->telemetry.frame;

    frame->frontend_active = 1U;
    frame->lock_cycles = 0U;
    if (frame->sampled)
        frame->frontend_stamp = pmccntr();
}

static inline void gl_telemetry_frontend_end(svc_state_t *st)
{
    zzapp_gl_frame_telemetry_t *frame = &st->telemetry.frame;

    if (frame->sampled && frame->frontend_active) {
        uint32_t cycles = pmccntr() - frame->frontend_stamp;

        counter_add(&frame->frontend, cycles - frame->lock_cycles);
    }
    frame->frontend_active = 0U;
}

void zzapp_svc_gl_telemetry_lock_begin(uint32_t pid)
{
    svc_state_t *st = svc_state(pid);
    zzapp_gl_frame_telemetry_t *frame;

    if (!st)
        return;
    frame = &st->telemetry.frame;
    if (frame->frontend_active && frame->sampled)
        frame->lock_stamp = pmccntr();
}

void zzapp_svc_gl_telemetry_lock_end(uint32_t pid, int contended)
{
    svc_state_t *st = svc_state(pid);
    zzapp_gl_frame_telemetry_t *frame;

    if (!st)
        return;
    frame = &st->telemetry.frame;
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

void zzapp_svc_gl_telemetry_reset(uint32_t pid)
{
    svc_state_t *st = svc_state(pid);

    if (!st)
        return;
    memset(&st->telemetry, 0, sizeof(st->telemetry));
    memset(&st->block.counters[1], 0, sizeof(st->block.counters[1]));
}

int zzapp_svc_gl_telemetry_swap(uint32_t pid, uint64_t now_us,
                                 zzapp_gl_telemetry_snapshot_t *snapshot)
{
    svc_state_t *st = svc_state(pid);
    zzapp_gl_telemetry_t *telemetry;
    zzapp_gl_frame_telemetry_t *frame;
    zzapp_gl_report_telemetry_t *report;
    zzapp_svc_counters_t *boundary;
    zzapp_gl_telemetry_snapshot_t local;
    zzapp_gl_telemetry_snapshot_t *out = snapshot ? snapshot : &local;
    uint32_t i;

    if (!st)
        return 0;
    telemetry = &st->telemetry;
    frame = &telemetry->frame;
    report = &telemetry->pending;
    boundary = &st->block.counters[1];
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

void zzapp_svc_gl_telemetry_report(uint32_t pid,
                                    const zzapp_gl_telemetry_snapshot_t *snapshot)
{
#ifndef ZZAPP_HOST_TEST
    if (!snapshot)
        return;
    printf("[app] gl slot=%lu frames=%lu calls=%lu/%lu top=%lu:%lu,%lu:%lu,"
           "%lu:%lu,%lu:%lu,%lu:%lu,%lu:%lu,%lu:%lu,%lu:%lu lock=%lu/%lu "
           "fail=%lu/%lu span=%lu/%lu avg=%lu/%lu/%lu/%lu share=%lu/%lu/%lu%%\r\n",
           (unsigned long)pid, (unsigned long)snapshot->frames,
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
    (void)pid;
    (void)snapshot;
#endif
}
#else
static inline void gl_telemetry_frame_begin(svc_state_t *st, uint32_t number)
{ (void)st; (void)number; }
static inline void gl_telemetry_descriptor_end(svc_state_t *st)
{ (void)st; }
static inline void gl_telemetry_descriptor_failure(svc_state_t *st)
{ (void)st; }
static inline void gl_telemetry_pointer_failure(svc_state_t *st)
{ (void)st; }
static inline void gl_telemetry_array_span(svc_state_t *st)
{ (void)st; }
static inline void gl_telemetry_array_fallback(svc_state_t *st)
{ (void)st; }
static inline void gl_telemetry_frontend_begin(svc_state_t *st)
{ (void)st; }
static inline void gl_telemetry_frontend_end(svc_state_t *st)
{ (void)st; }
#endif


#define P(a, d, l, n, e, c) { a, d, l, n, e, c, 0U, 0U, 0U }
#define N P(0U, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_FIXED, 0U, 0U, 0U)
#define D(a, r, p0, p1, p2) { a, 0U, r, 0U, 0U, { p0, p1, p2 } }
#define DP(a, r, p0, p1, p2) { a, 0U, r, 1U, 0U, { p0, p1, p2 } }
#define DF(a, v, r, p0, p1, p2) { a, v, r, 0U, 0U, { p0, p1, p2 } }
#define DL(a, r, p0, p1, p2) { a, 0U, r, 0U, ZZAPP_SVC_FAST_NO_LOCK, \
                                { p0, p1, p2 } }
#define DN(a, p0, p1) { a, 0U, ZZAPP_SVC_RETURN_INT, 0U, \
                        ZZAPP_SVC_COPY_OUT_ON_NOSPC, { p0, p1, N } }
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
#define OUT_GL_GET(a, e) P(a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_GL_GET, 0U, e, 1U)
#define IN_GL_VECTOR(a, p) P(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_GL_VECTOR, p, sizeof(float), 1U)
#define IN_INDICES(a, n) P(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_GL_INDICES, n, 1U, 0U)
/* Pointers a NULL value of which the API defines as AAR_E_INVAL (or as a
 * valid "none"): dispatch skips them, the handler decides. */
#define OPT(a, d, l, n, e, c) { a, d, l, n, e, c, 1U, 0U, 0U }
#define IN_OPT(a, n) OPT(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_FIXED, 0U, n, 1U)
#define IO_OPT(a, n) OPT(a, ZZAPP_SVC_POINTER_INOUT, ZZAPP_SVC_LENGTH_FIXED, 0U, n, 1U)
#define IO_SIZED(a, n) OPT(a, ZZAPP_SVC_POINTER_INOUT, ZZAPP_SVC_LENGTH_SIZE_FIELD, 0U, n, 1U)
#define IN_ARG_OPT(a, n) OPT(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_ARGUMENT, n, 1U, 0U)
#define OUT_ARG_OPT(a, n) OPT(a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_ARGUMENT, n, 1U, 0U)
#define IN_ARG64_OPT(a, n) OPT(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_ARGUMENT64, n, 1U, 0U)
#define OUT_ARG64_OPT(a, n) OPT(a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_ARGUMENT64, n, 1U, 0U)
#define IN_COUNT_OPT(a, n, e) OPT(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_COUNT, n, e, 0U)
#define OUT_COUNT_OPT(a, n, e) OPT(a, ZZAPP_SVC_POINTER_OUT, ZZAPP_SVC_LENGTH_COUNT, n, e, 0U)
#define IN_STRING_OPT(a) OPT(a, ZZAPP_SVC_POINTER_IN, ZZAPP_SVC_LENGTH_STRING, 0U, 1U, 1U)
#define OUT_PTR(a) OUT_OPTIONAL(a, sizeof(void *), 1U)
#define OUT_U32(a) OUT_OPTIONAL(a, 4U, 1U)
#define OUT_U64(a) OUT_OPTIONAL(a, 8U, 1U)

/* The descriptors are the ABI audit of aar_api. `arguments` counts AAPCS
 * words: a uint64_t or int64_t takes an even register pair or an 8-aligned
 * stack pair, so a call that has one after an odd number of words carries a
 * padding word (file_seek: fd, pad, offset lo and hi, whence, out_pos).
 * Every user pointer has its direction and extent; a pointer whose NULL
 * the API defines is optional here and refused by the handler. */
static const zzapp_svc_descriptor_t api_descriptors[API_VENEERS] = {
    /* core: lifecycle and time */
    [API_SLOT(exit)] = D(1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(yield)] = D(0, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(should_stop)] = DL(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(time_us)] = DL(0, ZZAPP_SVC_RETURN_U64, N, N, N),
    [API_SLOT(sleep_us)] = D(1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(info)] = D(1, ZZAPP_SVC_RETURN_INT, IO_SIZED(0, sizeof(aar_info)), N, N),
    /* core: private heap */
    [API_SLOT(heap_range)] = D(2, ZZAPP_SVC_RETURN_VOID, OUT_PTR(0), OUT_U64(1), N),
    [API_SLOT(heap_grow)] = D(3, ZZAPP_SVC_RETURN_INT, OUT_PTR(2), N, N),
    [API_SLOT(heap_shrink)] = D(3, ZZAPP_SVC_RETURN_INT, OUT_PTR(2), N, N),
    [API_SLOT(heap_info)] = D(1, ZZAPP_SVC_RETURN_INT, OUT_OPTIONAL(0, sizeof(aar_heap_info), 1U), N, N),
    /* core: shared buffers and visibility. The pointers of shared_free,
     * publish and acquire are identities looked up in the app's blocks. */
    [API_SLOT(shared_alloc)] = D(5, ZZAPP_SVC_RETURN_INT, OUT_OPTIONAL(4, sizeof(aar_shared), 1U), N, N),
    [API_SLOT(shared_free)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(publish)] = D(4, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(acquire)] = D(4, ZZAPP_SVC_RETURN_INT, N, N, N),
    /* core: log, files, console */
    [API_SLOT(log)] = D(2, ZZAPP_SVC_RETURN_VOID, IN_ARG(0, 1), N, N),
    [API_SLOT(file_open)] = D(2, ZZAPP_SVC_RETURN_INT, IN_STRING_OPT(0), N, N),
    [API_SLOT(file_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(file_read)] = D(4, ZZAPP_SVC_RETURN_INT, OUT_ARG(1, 2), OUT_U32(3), N),
    [API_SLOT(file_write)] = D(4, ZZAPP_SVC_RETURN_INT, IN_ARG(1, 2), OUT_U32(3), N),
    [API_SLOT(file_seek)] = D(6, ZZAPP_SVC_RETURN_INT, OUT_U64(5), N, N),
    [API_SLOT(file_stat)] = D(2, ZZAPP_SVC_RETURN_INT, IN_STRING_OPT(0), OUT_OPTIONAL(1, sizeof(aar_stat), 1U), N),
    [API_SLOT(file_list_open)] = D(1, ZZAPP_SVC_RETURN_INT, IN_STRING_OPT(0), N, N),
    [API_SLOT(file_list_next)] = D(2, ZZAPP_SVC_RETURN_INT, OUT_OPTIONAL(1, sizeof(aar_dirent), 1U), N, N),
    [API_SLOT(file_list_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(console_write)] = D(3, ZZAPP_SVC_RETURN_INT, IN_ARG(0, 1), OUT_U32(2), N),
    [API_SLOT(console_read)] = D(3, ZZAPP_SVC_RETURN_INT, OUT_ARG(0, 1), OUT_U32(2), N),
    /* core: events and messages */
    [API_SLOT(event_poll)] = D(3, ZZAPP_SVC_RETURN_INT, OUT_COUNT_OPT(0, 1, sizeof(aar_event)), OUT_U32(2), N),
    [API_SLOT(msg_send)] = D(1, ZZAPP_SVC_RETURN_INT, IN_OPT(0, AAR_MSG_BYTES), N, N),
    [API_SLOT(msg_poll)] = DP(1, ZZAPP_SVC_RETURN_INT, OUT_OPTIONAL(0, AAR_MSG_BYTES, 1U), N, N),
    [API_SLOT(msg_wait)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    /* core: file mutations, host environment and clipboard */
    [API_SLOT(file_delete)] = D(1, ZZAPP_SVC_RETURN_INT, IN_STRING_OPT(0), N, N),
    [API_SLOT(file_rename)] = D(3, ZZAPP_SVC_RETURN_INT, IN_STRING_OPT(0), IN_STRING_OPT(1), N),
    [API_SLOT(dir_create)] = D(1, ZZAPP_SVC_RETURN_INT, IN_STRING_OPT(0), N, N),
    [API_SLOT(file_set_date)] = D(4, ZZAPP_SVC_RETURN_INT, IN_STRING_OPT(0), N, N),
    [API_SLOT(file_set_protect)] = D(2, ZZAPP_SVC_RETURN_INT, IN_STRING_OPT(0), N, N),
    [API_SLOT(file_truncate)] = D(4, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(volume_info)] = D(2, ZZAPP_SVC_RETURN_INT, IN_STRING_OPT(0), IO_SIZED(1, sizeof(aar_volume_info)), N),
    [API_SLOT(time_unix_us)] = D(1, ZZAPP_SVC_RETURN_INT, OUT_U64(0), N, N),
    [API_SLOT(locale_info)] = D(1, ZZAPP_SVC_RETURN_INT, IO_SIZED(0, sizeof(aar_locale_info)), N, N),
    [API_SLOT(env_get)] = { 4, 0U, ZZAPP_SVC_RETURN_INT, 0U, ZZAPP_SVC_COPY_OUT_ON_NOSPC,
                           { IN_STRING_OPT(0), OUT_ARG_OPT(1, 2), OUT_U32(3) } },
    [API_SLOT(clip_read)] = DN(5, OUT_ARG_OPT(2, 3), OUT_U32(4)),
    [API_SLOT(clip_write)] = D(4, ZZAPP_SVC_RETURN_INT, IN_ARG_OPT(2, 3), N, N),
    /* optional */
    [API_SLOT(ext)] = D(2, ZZAPP_SVC_RETURN_POINTER, N, N, N),
    [API_SLOT(map_file)] = D(3, ZZAPP_SVC_RETURN_INT, IN_STRING_OPT(0), OUT_PTR(1), OUT_U64(2)),
    [API_SLOT(unmap_file)] = D(1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(file_get_comment)] = { 4, 0U, ZZAPP_SVC_RETURN_INT, 0U, ZZAPP_SVC_COPY_OUT_ON_NOSPC,
                                     { IN_STRING_OPT(0), OUT_ARG_OPT(1, 2), OUT_U32(3) } },
    [API_SLOT(file_set_comment)] = D(3, ZZAPP_SVC_RETURN_INT, IN_STRING_OPT(0), IN_ARG_OPT(1, 2), N),
    /* display */
    [API_SLOT(display_open)] = D(5, ZZAPP_SVC_RETURN_INT, OUT_OPTIONAL(4, sizeof(aar_surface), 1U), N, N),
    [API_SLOT(display_flip)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_flip_async)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_flip_idle)] = D(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_refresh)] = D(1, ZZAPP_SVC_RETURN_INT, IO_OPT(0, sizeof(aar_surface)), N, N),
    [API_SLOT(display_vsync)] = D(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_set_palette)] = D(4, ZZAPP_SVC_RETURN_INT, IN_COUNT_OPT(3, 2, 4U), N, N),
    [API_SLOT(display_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_latest_dropped)] = D(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_info)] = D(2, ZZAPP_SVC_RETURN_INT, IO_SIZED(1, sizeof(aar_display_info)), N, N),
    [API_SLOT(display_modes)] = DN(3, OUT_COUNT_OPT(0, 1, sizeof(aar_display_mode)), OUT_U32(2)),
    [API_SLOT(display_resize)] = D(4, ZZAPP_SVC_RETURN_INT, OUT_OPTIONAL(3, sizeof(aar_surface), 1U), N, N),
    [API_SLOT(display_set_title)] = D(2, ZZAPP_SVC_RETURN_INT, IN_STRING_OPT(1), N, N),
    [API_SLOT(display_pointer)] = D(3, ZZAPP_SVC_RETURN_INT, IN_OPT(2, sizeof(aar_pointer)), N, N),
    [API_SLOT(display_wait_vblank)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(display_last_present_us)] = D(0, ZZAPP_SVC_RETURN_U64, N, N, N),
    /* audio */
    [API_SLOT(audio_open)] = D(3, ZZAPP_SVC_RETURN_INT, OUT_OPTIONAL(2, sizeof(aar_audio), 1U), N, N),
    [API_SLOT(audio_close)] = D(0, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(audio_stats)] = D(1, ZZAPP_SVC_RETURN_INT, IO_SIZED(0, sizeof(aar_audio_stats)), N, N),
    /* GL */
    [API_SLOT(gl_bind)] = D(1, ZZAPP_SVC_RETURN_POINTER, N, N, N),
    [API_SLOT(gl_swap)] = D(0, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [API_SLOT(gl_unbind)] = D(0, ZZAPP_SVC_RETURN_VOID, N, N, N),
    /* services: whole-buffer byte counts are uint64_t, so every buffer
     * whose length is one takes its length from a word pair */
    [API_SLOT(sha256)] = D(5, ZZAPP_SVC_RETURN_INT, IN_ARG64_OPT(0, 2), OUT_OPTIONAL(4, AAR_SHA256_BYTES, 1U), N),
    [API_SLOT(decompress)] = D(11, ZZAPP_SVC_RETURN_INT, IN_ARG64_OPT(2, 4), OUT_ARG64_OPT(6, 8), OUT_OPTIONAL(10, sizeof(aar_service_result), 1U)),
    [API_SLOT(image_open)] = D(3, ZZAPP_SVC_RETURN_INT, IN_OPT(0, sizeof(aar_image_params)), OUT_U32(1), OUT_OPTIONAL(2, sizeof(aar_image_result), 1U)),
    [API_SLOT(image_decode)] = D(5, ZZAPP_SVC_RETURN_INT, IN_ARG_OPT(1, 2), OUT_OPTIONAL(4, sizeof(aar_image_result), 1U), N),
    [API_SLOT(image_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    [API_SLOT(audio_decode_open)] = D(2, ZZAPP_SVC_RETURN_INT, OUT_U32(1), N, N),
    [API_SLOT(audio_decode)] = D(7, ZZAPP_SVC_RETURN_INT, IN_ARG_OPT(1, 2), OUT_COUNT_OPT(4, 5, 4U), OUT_OPTIONAL(6, sizeof(aar_audio_decode_result), 1U)),
    [API_SLOT(audio_decode_close)] = D(1, ZZAPP_SVC_RETURN_INT, N, N, N),
    /* optional async files */
    [API_SLOT(file_read_async)] = D(4, ZZAPP_SVC_RETURN_INT, OUT_ARG_OPT(1, 2), OUT_U32(3), N),
    [API_SLOT(file_write_async)] = D(4, ZZAPP_SVC_RETURN_INT, IN_ARG_OPT(1, 2), OUT_U32(3), N),
    [API_SLOT(file_collect)] = D(3, ZZAPP_SVC_RETURN_INT, OUT_U32(1), OUT_U32(2), N),
    /* aar_ext_zz9000.gl_bind_flags: not a table slot, the next veneer */
    [ZZAPP_SVC_EXT_GL_BIND_FLAGS] = D(2, ZZAPP_SVC_RETURN_POINTER, N, N, N)
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
    [GL_SLOT(GetIntegerv)] = D(2, ZZAPP_SVC_RETURN_VOID, OUT_GL_GET(1, 4U), N, N),
    [GL_SLOT(GetFloatv)] = D(2, ZZAPP_SVC_RETURN_VOID, OUT_GL_GET(1, 4U), N, N),
    G0(GetError, ZZAPP_SVC_RETURN_INT), G2(Hint, ZZAPP_SVC_RETURN_VOID),
    G2(PixelStorei, ZZAPP_SVC_RETURN_VOID), DF(0, 8, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(Build2DMipmaps)] = D(7, ZZAPP_SVC_RETURN_INT, IN_PIXELS(6, 2, 4, 5), N, N),
    /* The array pointers are not checked when they are set: gl_validate_draw
     * checks every enabled array when it is drawn. */
    G4(VertexPointer, ZZAPP_SVC_RETURN_VOID), G4(TexCoordPointer, ZZAPP_SVC_RETURN_VOID),
    G4(ColorPointer, ZZAPP_SVC_RETURN_VOID),
    G1(EnableClientState, ZZAPP_SVC_RETURN_VOID), G1(DisableClientState, ZZAPP_SVC_RETURN_VOID),
    G1(ClientActiveTextureARB, ZZAPP_SVC_RETURN_VOID), G3(DrawArrays, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(DrawElements)] = D(4, ZZAPP_SVC_RETURN_VOID, IN_INDICES(3, 1), N, N),
    G2(LockArrays, ZZAPP_SVC_RETURN_VOID), G0(UnlockArrays, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(ClearDepth)] = DF(0, 2, ZZAPP_SVC_RETURN_VOID, N, N, N),
    G4(ColorMask, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(PolygonOffset)] = DF(0, 2, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(MultMatrixf)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 64U, 1U), N, N),
    [GL_SLOT(LoadMatrixd)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 128U, 1U), N, N),
    [GL_SLOT(MultMatrixd)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 128U, 1U), N, N),
    [GL_SLOT(GetDoublev)] = D(2, ZZAPP_SVC_RETURN_VOID, OUT_GL_GET(1, 8U), N, N),
    [GL_SLOT(GetBooleanv)] = D(2, ZZAPP_SVC_RETURN_VOID, OUT_GL_GET(1, 1U), N, N),
    G1(ActiveTextureARB, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(MultiTexCoord2fARB)] = DF(1, 2, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(MultiTexCoord2fvARB)] = D(2, ZZAPP_SVC_RETURN_VOID, IN_FIXED(1, 8U, 1U), N, N),
    [GL_SLOT(Normal3f)] = DF(0, 3, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(Normal3fv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 12U, 1U), N, N),
    G3(NormalPointer, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(Lightf)] = DF(2, 1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(Lightfv)] = D(3, ZZAPP_SVC_RETURN_VOID, IN_GL_VECTOR(2, 1), N, N),
    [GL_SLOT(LightModelf)] = DF(1, 1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(LightModelfv)] = D(2, ZZAPP_SVC_RETURN_VOID, IN_GL_VECTOR(1, 0), N, N),
    G2(LightModeli, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(Materialf)] = DF(2, 1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(Materialfv)] = D(3, ZZAPP_SVC_RETURN_VOID, IN_GL_VECTOR(2, 1), N, N),
    G2(ColorMaterial, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(Fogf)] = DF(1, 1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(Fogfv)] = D(2, ZZAPP_SVC_RETURN_VOID, IN_GL_VECTOR(1, 0), N, N),
    G2(Fogi, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(LineWidth)] = DF(0, 1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    [GL_SLOT(PointSize)] = DF(0, 1, ZZAPP_SVC_RETURN_VOID, N, N, N),
    G3(Color3ub, ZZAPP_SVC_RETURN_VOID), G2(Vertex2i, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(Vertex2fv)] = D(1, ZZAPP_SVC_RETURN_VOID, IN_FIXED(0, 8U, 1U), N, N),
    G2(TexCoord2i, ZZAPP_SVC_RETURN_VOID), G1(IsTexture, ZZAPP_SVC_RETURN_INT),
    G8(CopyTexImage2D, ZZAPP_SVC_RETURN_VOID), G8(CopyTexSubImage2D, ZZAPP_SVC_RETURN_VOID),
    [GL_SLOT(ClipPlane)] = D(2, ZZAPP_SVC_RETURN_VOID, IN_FIXED(1, 32U, 1U), N, N),
    [GL_SLOT(TexEnvfv)] = D(3, ZZAPP_SVC_RETURN_VOID, IN_GL_VECTOR(2, 1), N, N)
};

uint32_t zzapp_svc_api_count(void) { return API_COUNT; }
uint32_t zzapp_svc_gl_count(void) { return GL_COUNT; }
const zzapp_svc_descriptor_t *zzapp_svc_api_descriptor(uint32_t slot)
{ return slot < API_VENEERS ? &api_descriptors[slot] : NULL; }
const zzapp_svc_descriptor_t *zzapp_svc_gl_descriptor(uint32_t slot)
{ return slot < GL_COUNT ? &gl_descriptors[slot] : NULL; }

uintptr_t zzapp_svc_api_veneer(uint32_t slot)
{ return slot < API_VENEERS ? (uintptr_t)VENEER_PAGE + slot * VENEER_BYTES : 0U; }
uintptr_t zzapp_svc_gl_veneer(uint32_t slot)
{ return slot < GL_COUNT ? (uintptr_t)VENEER_PAGE + (ZZAPP_SVC_GL_FIRST + slot) * VENEER_BYTES : 0U; }
int zzapp_svc_set_veneer_alias(uint32_t pid, uintptr_t address)
{
    svc_state_t *st = svc_state(pid);

    if (!st || !address || (uint64_t)address > UINT32_MAX ||
        address & (VENEER_PAGE_BYTES - 1U))
        return AAR_E_INVAL;
    st->veneer_alias = (uint32_t)address;
    return AAR_OK;
}
int zzapp_svc_set_table_aliases(uint32_t pid, uintptr_t api_address,
                                uintptr_t gl_address)
{
    svc_state_t *st = svc_state(pid);

    if (!st || !api_address || !gl_address ||
        (uint64_t)api_address > UINT32_MAX || (uint64_t)gl_address > UINT32_MAX)
        return AAR_E_INVAL;
    st->api_alias = (uint32_t)api_address;
    st->gl_alias = (uint32_t)gl_address;
    return AAR_OK;
}
int zzapp_svc_is_veneer(uintptr_t address)
{
    uintptr_t first = (uintptr_t)VENEER_PAGE;
    uintptr_t end = first + (ZZAPP_SVC_GL_FIRST + GL_COUNT) * VENEER_BYTES;
    return address >= first && address < end && (address - first) % VENEER_BYTES == 0U;
}

static void copy_table_string(char *destination, size_t capacity,
                              const char *source)
{
    uint32_t i;
    if (!source) { destination[0] = '\0'; return; }
    for (i = 0U; i + 1U < capacity && source[i]; ++i)
        destination[i] = source[i];
    destination[i] = '\0';
}

/* A veneer, or NULL where the runtime's table has a NULL slot: the app
 * reads NULL as "not implemented" (audio_stats). Slots the source table's
 * size does not cover get a veneer; the app gates on `size`. */
static uintptr_t table_entry(const aar_api *source, uint32_t slot, uintptr_t veneer)
{
    uint64_t end = (uint64_t)API_FIRST + ((uint64_t)slot + 1U) * sizeof(void *);

    if (end <= source->size &&
        *(const uintptr_t *)(const void *)((const uint8_t *)source + API_FIRST +
                                           slot * sizeof(void *)) == 0U)
        return 0U;
    return veneer;
}

int zzapp_svc_copy_api(aar_api *destination, const aar_api *source)
{
    uintptr_t *entries;
    uint32_t i;
    if (!destination || !source || source->size > sizeof(*destination) || source->size < API_FIRST)
        return AAR_E_INVAL;
    memcpy(destination, source, source->size);
    entries = (uintptr_t *)(void *)((uint8_t *)destination + API_FIRST);
    for (i = 0U; i < API_COUNT; ++i)
        entries[i] = table_entry(source, i, zzapp_svc_api_veneer(i));
    return AAR_OK;
}

/* Where the app sees field `offset` of its table page. */
static uintptr_t user_table_address(const svc_state_t *st,
                                    const void *kernel_address, size_t offset)
{
    return st->api_alias ? (uintptr_t)st->api_alias + offset :
           (uintptr_t)kernel_address;
}

static uintptr_t user_veneer(const svc_state_t *st, uint32_t number)
{
    return (st->veneer_alias ? (uintptr_t)st->veneer_alias :
                               (uintptr_t)VENEER_PAGE) +
           number * VENEER_BYTES;
}

/* The app's aar_ext_zz9000: the runtime's own table with
 * gl_bind_flags a veneer and build_id copied into the app's read-only page.
 * gl_build_id stays NULL: the resident firmware does not know the GL module's
 * source revision (the module prints it on the UART when it loads). */
static void fill_user_ext(const svc_state_t *st, struct user_api_table *table,
                          const aar_api *source)
{
    const aar_ext_zz9000 *runtime = NULL;

    memset(&table->ext, 0, sizeof(table->ext));
    table->build_id[0] = '\0';
    if (AAR_API_COVERS(source, ext) && source->ext)
        runtime = (const aar_ext_zz9000 *)source->ext(AAR_PLATFORM_ZZ9000,
                                                       AAR_EXT_ZZ9000_MAIN);
    if (!runtime)
        return;
    table->ext.size = (uint32_t)sizeof(table->ext);
    if (AAR_EXT_ZZ9000_COVERS(runtime, gl_bind_flags) && runtime->gl_bind_flags)
        table->ext.gl_bind_flags = (const aar_gl *(*)(uint32_t, uint32_t))
            user_veneer(st, ZZAPP_SVC_EXT_GL_BIND_FLAGS);
    if (AAR_EXT_ZZ9000_COVERS(runtime, build_id) && runtime->build_id) {
        copy_table_string(table->build_id, sizeof(table->build_id),
                          runtime->build_id);
        table->ext.build_id = (const char *)user_table_address(st,
            table->build_id, offsetof(struct user_api_table, build_id));
    }
}

/* Both table pages are the app's read-only view of its record's tables area,
 * and the area is reused from one app to the next: start from zero, so no
 * byte of the previous app's tables outlives its table size. */
const aar_api *zzapp_svc_api_for_slot(uint32_t pid, const aar_api *source)
{
    svc_state_t *st = svc_state(pid);
    struct user_api_table *table = user_api_of(pid);
    struct user_gl_table *gl_table = user_gl_of(pid);
    uintptr_t *entries;
    uint32_t i;
    if (!st || !table || !gl_table || !source || source->size > sizeof(*source) || source->size < API_FIRST)
        return NULL;
    memset(table, 0, sizeof(*table));
    memset(gl_table, 0, sizeof(*gl_table));
    memcpy(&table->table, source, source->size);
    entries = (uintptr_t *)(void *)((uint8_t *)&table->table + API_FIRST);
    for (i = 0U; i < API_COUNT; ++i)
        entries[i] = table_entry(source, i, user_veneer(st, i));
    fill_user_ext(st, table, source);
    return &table->table;
}

/* What the `ext` call hands the app: its table, or NULL. */
const aar_ext_zz9000 *zzapp_svc_ext_for_slot(uint32_t pid)
{
    struct user_api_table *table = user_api_of(pid);

    return table && table->ext.size ? &table->ext : NULL;
}

static uintptr_t ext_user_address(const svc_state_t *st,
                                  const struct user_api_table *table)
{
    return user_table_address(st, &table->ext,
                              offsetof(struct user_api_table, ext));
}
aar_gl *zzapp_svc_gl_for_slot(uint32_t pid)
{
    struct user_gl_table *table = user_gl_of(pid);

    return table ? &table->table : NULL;
}
int zzapp_svc_copy_gl(aar_gl *destination, const aar_gl *source)
{
    uintptr_t *entries;
    const uintptr_t *direct;
    uint32_t i, count;

    if (!destination || !source || source->size > sizeof(*destination) ||
        source->size < GL_FIRST || (source->size - GL_FIRST) % sizeof(void *))
        return AAR_E_INVAL;
    destination->size = source->size;
    destination->version = source->version;
    count = (source->size - GL_FIRST) / sizeof(void *);
    entries = (uintptr_t *)(void *)((uint8_t *)destination + GL_FIRST);
    direct = (const uintptr_t *)(const void *)((const uint8_t *)source + GL_FIRST);
    for (i = 0U; i < count; ++i)
        entries[i] = direct[i] ? zzapp_svc_gl_veneer(i) : 0U;
    memset(entries + count, 0, (GL_COUNT - count) * sizeof(*entries));
    return AAR_OK;
}
static int zzapp_svc_copy_gl_for_slot(const svc_state_t *st, aar_gl *destination,
                                      const aar_gl *source)
{
    uintptr_t *entries;
    uint32_t i;

    if (zzapp_svc_copy_gl(destination, source) != AAR_OK)
        return AAR_E_INVAL;
    if (!st->veneer_alias)
        return AAR_OK;
    entries = (uintptr_t *)(void *)((uint8_t *)destination + GL_FIRST);
    for (i = 0U; i < GL_COUNT; ++i)
        if (entries[i])
            entries[i] = (uintptr_t)st->veneer_alias +
                         (ZZAPP_SVC_GL_FIRST + i) * VENEER_BYTES;
    return AAR_OK;
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
    if (!as || !as->active) return AAR_E_FAULT;
    if (!bytes) return AAR_OK;
    end = (uint64_t)address + bytes;
    if (end > UINT32_MAX + 1ULL) return AAR_E_FAULT;
    for (page = address & ~(uint64_t)(PAGE_BYTES - 1U); page < end; page += PAGE_BYTES) {
        uint32_t address_page = (uint32_t)page;
        uint32_t descriptor = as->l1[address_page >> 20];
        if ((descriptor & L1_TYPE_MASK) == L1_TABLE) {
            const uint32_t *l2 = zzapp_as_l2_for(as, address_page);
            if (!l2 || !page_allows(l2[(address_page >> 12) & 0xffU], write)) return AAR_E_FAULT;
        } else if (!section_allows(descriptor, write)) return AAR_E_FAULT;
    }
    return AAR_OK;
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
static void gl_client_reset(svc_state_t *st)
{
    memset(&st->gl_client, 0, sizeof(st->gl_client));
    st->gl_client.pack.alignment = 4U;
    st->gl_client.unpack.alignment = 4U;
}
static void gl_release(svc_state_t *st, uint32_t generation)
{
    if (st->gl_client.generation == generation) {
        st->direct_gl = NULL;
        gl_client_reset(st);
    }
}
void zzapp_svc_gl_release(uint32_t pid, uint32_t generation)
{
    svc_state_t *st = svc_state(pid);

    if (st)
        gl_release(st, generation);
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
static void gl_record_client_call(svc_state_t *st, uint32_t number,
                                  const uint32_t args[SVC_MAX_WORDS])
{
    gl_client_state_t *state = &st->gl_client;
    gl_client_array_t *array;

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
    if (number == GL_SLOT(NormalPointer)) {
        if (!state->in_begin) {
            array = &state->normal;
            array->size = args[0] == GL_FLOAT && (int32_t)args[1] >= 0 &&
                          args[2] ? 3 : 0;
            array->type = GL_FLOAT;
            array->stride = (int32_t)args[1];
            array->pointer = args[2];
        }
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
        else if (args[0] == GL_NORMAL_ARRAY) state->normal.enabled = enabled;
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
           zzapp_svc_check_user(as, (uint32_t)address, element_bytes, 0) == AAR_OK;
}
static int gl_validate_array_index(const zzapp_as_t *as,
                                   const gl_client_state_t *state,
                                   uint32_t index)
{
    uint32_t texture;

    if (!gl_validate_array_element(as, &state->vertex, index) ||
        !gl_validate_array_element(as, &state->color, index) ||
        !gl_validate_array_element(as, &state->normal, index))
        return 0;
    for (texture = 0U; texture < GL_TEXTURE_UNITS; ++texture)
        if (!gl_validate_array_element(as, &state->texcoord[texture], index))
            return 0;
    return 1;
}
static int gl_validate_array_span(svc_state_t *st, const zzapp_as_t *as,
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
    gl_telemetry_array_span(st);
    return zzapp_svc_check_user(as, (uint32_t)lo, (uint32_t)(hi - lo), 0) ==
           AAR_OK;
}
static int gl_validate_spans(svc_state_t *st, const zzapp_as_t *as,
                             gl_client_state_t *state, uint32_t min,
                             uint32_t max, uint32_t element_count)
{
    uint32_t texture;

    if (!gl_validate_array_span(st, as, &state->vertex, min, max, element_count) ||
        !gl_validate_array_span(st, as, &state->color, min, max, element_count) ||
        !gl_validate_array_span(st, as, &state->normal, min, max, element_count))
        return 0;
    for (texture = 0U; texture < GL_TEXTURE_UNITS; ++texture)
        if (!gl_validate_array_span(st, as, &state->texcoord[texture],
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
static int gl_validate_draw(const zzapp_as_t *as, svc_state_t *st,
                            uint32_t number, const uint32_t args[SVC_MAX_WORDS])
{
    gl_client_state_t *state = &st->gl_client;
    int32_t count;
    uint32_t min;
    uint32_t max;
    int span_status;

    uint32_t i;
    uint32_t index_bytes = 0U;
    const uint8_t *indices = NULL;

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
    span_status = gl_validate_spans(st, as, state, min, max,
                                    (uint32_t)count);
    if (span_status == 1)
        return 1;
    if (span_status < 0)
        return 0;
    gl_telemetry_array_fallback(st);
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
    if (!value || address < stack || zzapp_svc_check_user(as, address, 4U, 0) != AAR_OK) return AAR_E_FAULT;
    *value = *(const uint32_t *)(uintptr_t)address;
    return AAR_OK;
}
static int frame_arguments(const zzapp_as_t *as, const zzapp_svc_frame_t *frame,
                           uint32_t words, uint32_t args[SVC_MAX_WORDS])
{
    uint32_t i;
    if (words > SVC_MAX_WORDS) return AAR_E_FAULT;
    for (i = 0U; i < words && i < 4U; ++i) args[i] = frame->r[i];
    for (; i < words; ++i)
        if (stack_word(as, frame->user_sp, i - 4U, &args[i]) != AAR_OK) return AAR_E_FAULT;
    return AAR_OK;
}
static int gl_pixel_bytes(const svc_state_t *st, const uint32_t args[SVC_MAX_WORDS],
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

    if (width <= 0 || height <= 0) {
        *bytes = 0U;
        return AAR_OK;
    }
    switch (format) {
    case 0x1903U: case 0x1906U: case 0x1909U: components = 1U; break;
    case 0x190aU: components = 2U; break;
    case 0x1907U: case 0x80e0U: components = 3U; break;
    case 0x1908U: case 0x80e1U: components = 4U; break;
    default: return AAR_E_FAULT;
    }
    switch (type) {
    case 0x1400U: case 0x1401U: element = 1U; break;
    case 0x1402U: case 0x1403U: element = 2U; break;
    case 0x1404U: case 0x1405U: case 0x1406U: element = 4U; break;
    case 0x8033U: case 0x8363U: components = 1U; element = 2U; break;
    case 0x8034U: case 0x8035U: case 0x8367U: components = 1U; element = 4U; break;
    default: return AAR_E_FAULT;
    }
    store = pointer->direction == ZZAPP_SVC_POINTER_IN ?
        &st->gl_client.unpack : &st->gl_client.pack;
    pixel_bytes = (uint64_t)components * element;
    source_width = store->row_length ? store->row_length : (uint32_t)width;
    row_bytes = source_width * pixel_bytes;
    row_bytes = (row_bytes + store->alignment - 1U) &
                ~(uint64_t)(store->alignment - 1U);
    length = (uint64_t)store->skip_rows * row_bytes +
             (uint64_t)store->skip_pixels * pixel_bytes +
             ((uint64_t)(uint32_t)height - 1U) * row_bytes +
             (uint64_t)(uint32_t)width * pixel_bytes;
    if (length > UINT32_MAX) return AAR_E_FAULT;
    *bytes = (uint32_t)length;
    return AAR_OK;
}
static int gl_indices_bytes(const uint32_t args[SVC_MAX_WORDS], uint32_t first,
                            uint32_t *bytes)
{
    uint32_t element;
    uint64_t length;
    int32_t count = (int32_t)args[first];

    if (count <= 0) {
        *bytes = 0U;
        return AAR_OK;
    }
    switch (args[first + 1U]) {
    case 0x1401U: element = 1U; break;
    case 0x1403U: element = 2U; break;
    case 0x1405U: element = 4U; break;
    default: return AAR_E_FAULT;
    }
    length = (uint64_t)(uint32_t)count * element;
    if (length > UINT32_MAX) return AAR_E_FAULT;
    *bytes = (uint32_t)length;
    return AAR_OK;
}
static uint32_t gl_vector_values(uint32_t pname)
{
    switch (pname) {
    case 0x1204U: /* spot direction */
        return 3U;
    case 0x1200U: case 0x1201U: case 0x1202U: case 0x1203U:
    case 0x1600U: case 0x1602U: /* AMBIENT..POSITION, EMISSION, AMBIENT_AND_DIFFUSE */
    case 0x0B53U: case 0x0B66U: case 0x2201U: /* model ambient, fog/texenv color */
        return 4U;
    default:
        return 1U;
    }
}
/* Extent in bytes of one pointer argument. AAR_E_FAULT for a length no
 * address space holds, AAR_E_INVAL for a 64-bit service length the
 * firmware does not handle. */
static int pointer_bytes(const svc_state_t *st, const zzapp_svc_pointer_t *pointer,
                         const uint32_t args[SVC_MAX_WORDS], uint32_t *bytes)
{
    uint64_t length;

    if (pointer->length == ZZAPP_SVC_LENGTH_FIXED) length = pointer->element_bytes;
    else if (pointer->length == ZZAPP_SVC_LENGTH_ARGUMENT) length = args[pointer->length_argument];
    else if (pointer->length == ZZAPP_SVC_LENGTH_ARGUMENT64) {
        if (args[pointer->length_argument + 1U] != 0U) return AAR_E_INVAL;
        length = args[pointer->length_argument];
    }
    else if (pointer->length == ZZAPP_SVC_LENGTH_COUNT)
        length = (uint64_t)args[pointer->length_argument] * pointer->element_bytes;
    else if (pointer->length == ZZAPP_SVC_LENGTH_GL_PIXELS)
        return gl_pixel_bytes(st, args, pointer, bytes);
    else if (pointer->length == ZZAPP_SVC_LENGTH_GL_INDICES)
        return gl_indices_bytes(args, pointer->length_argument, bytes);
    else if (pointer->length == ZZAPP_SVC_LENGTH_GL_GET)
        length = zzapp_gl_get_values(args[0]) * pointer->element_bytes;
    else if (pointer->length == ZZAPP_SVC_LENGTH_GL_VECTOR)
        length = gl_vector_values(args[pointer->length_argument]) * sizeof(float);
    else return AAR_OK;
    if (length > UINT32_MAX) return AAR_E_FAULT;
    *bytes = (uint32_t)length;
    return AAR_OK;
}
static int copy_string(const zzapp_as_t *as, uint32_t address, char destination[STRING_BYTES])
{
    uint32_t i;
    if (!address) return AAR_E_FAULT;
    for (i = 0U; i < STRING_BYTES; ++i) {
        if (zzapp_svc_check_user(as, address + i, 1U, 0) != AAR_OK) return AAR_E_FAULT;
        destination[i] = *(const char *)(uintptr_t)(address + i);
        if (!destination[i]) return AAR_OK;
    }
    return AAR_E_FAULT;
}
typedef uint32_t (*word_call_t)(uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t, uint32_t, uint32_t, uint32_t);
/* The handler sees the words as AAPCS delivers them: r0-r3, then the rest
 * on the stack in order, a 64-bit value in an even register pair or at an
 * 8-aligned stack offset. */
static uint32_t call_words(uintptr_t address, const uint32_t args[SVC_MAX_WORDS])
{
    word_call_t call = (word_call_t)address;
    return call(args[0], args[1], args[2], args[3], args[4], args[5], args[6],
                args[7], args[8], args[9], args[10], args[11]);
}
static int entry_at(const void *table, uint32_t size, uint32_t first, uint32_t slot,
                    uintptr_t *entry)
{
    uint64_t end = (uint64_t)first + ((uint64_t)slot + 1U) * sizeof(void *);
    if (!table || end > size) return AAR_E_NOSYS;
    *entry = *(const uintptr_t *)(const void *)((const uint8_t *)table + first + slot * sizeof(void *));
    return *entry ? AAR_OK : AAR_E_NOSYS;
}

/* Checks every pointer of the call and swaps the copied ones for kernel
 * buffers. lengths[i] is the extent validated and copied for pointer i and
 * tails[i] the bytes past it that a size-first structure is zero-filled
 * with. Returns AAR_OK, AAR_E_FAULT or AAR_E_INVAL. */
static int prepare_pointers(const svc_state_t *st, const zzapp_as_t *as,
                            const zzapp_svc_descriptor_t *descriptor,
                            uint32_t args[SVC_MAX_WORDS], uint8_t copies[3][COPY_BYTES],
                            char strings[3][STRING_BYTES], uint32_t originals[3],
                            uint32_t lengths[3], uint32_t tails[3])
{
    uint32_t i;
    for (i = 0U; i < 3U; ++i) {
        const zzapp_svc_pointer_t *pointer = &descriptor->pointers[i];
        uint32_t bytes = 0U;
        uint32_t tail = 0U;
        uint32_t address;
        int write;
        int rc;
        lengths[i] = 0U;
        tails[i] = 0U;
        if (!pointer->element_bytes) continue;
        originals[i] = args[pointer->argument];
        if (pointer->optional && !originals[i])
            continue;
        address = args[pointer->argument];
        write = pointer->direction != ZZAPP_SVC_POINTER_IN;
        if (pointer->length == ZZAPP_SVC_LENGTH_STRING) {
            if (copy_string(as, address, strings[i]) != AAR_OK) return AAR_E_FAULT;
            args[pointer->argument] = (uint32_t)(uintptr_t)strings[i];
            continue;
        }
        if (pointer->length == ZZAPP_SVC_LENGTH_SIZE_FIELD) {
            uint32_t size;
            if (zzapp_svc_check_user(as, address, 4U, 0) != AAR_OK) return AAR_E_FAULT;
            memcpy(&size, (const void *)(uintptr_t)address, sizeof(size));
            if (zzapp_svc_check_user(as, address, size, write) != AAR_OK) return AAR_E_FAULT;
            bytes = size < pointer->element_bytes ? size : pointer->element_bytes;
            tail = size - bytes;
        } else {
            rc = pointer_bytes(st, pointer, args, &bytes);
            if (rc != AAR_OK) return rc;
            if ((!address && bytes) ||
                zzapp_svc_check_user(as, address, bytes, write) != AAR_OK) return AAR_E_FAULT;
        }
        if (pointer->copy) {
            if (bytes > COPY_BYTES) return AAR_E_FAULT;
            memset(copies[i], 0, COPY_BYTES);
            if (pointer->direction != ZZAPP_SVC_POINTER_OUT)
                memcpy(copies[i], (const void *)(uintptr_t)address, bytes);
            args[pointer->argument] = (uint32_t)(uintptr_t)copies[i];
        }
        lengths[i] = bytes;
        tails[i] = tail;
    }
    return AAR_OK;
}
static void finish_pointers(const zzapp_svc_descriptor_t *descriptor,
                            uint8_t copies[3][COPY_BYTES],
                            const uint32_t originals[3],
                            const uint32_t lengths[3], const uint32_t tails[3],
                            uint32_t result)
{
    uint32_t i;

    if (descriptor->result == ZZAPP_SVC_RETURN_INT && result != (uint32_t)AAR_OK &&
        (!descriptor->copy_out_on_positive || (int32_t)result <= 0) &&
        (!(descriptor->flags & ZZAPP_SVC_COPY_OUT_ON_NOSPC) ||
         result != (uint32_t)AAR_E_NOSPC))
        return;
    for (i = 0U; i < 3U; ++i) {
        const zzapp_svc_pointer_t *pointer = &descriptor->pointers[i];
        if (!pointer->element_bytes || !pointer->copy ||
            (pointer->optional && !originals[i]) ||
            pointer->direction == ZZAPP_SVC_POINTER_IN) continue;
        memcpy((void *)(uintptr_t)originals[i], copies[i], lengths[i]);
        if (tails[i])
            memset((uint8_t *)(uintptr_t)originals[i] + lengths[i], 0, tails[i]);
    }
}

/* The descriptor copies the image header; only the pixels named by a valid
 * CUSTOM header are read. The service copies them before parking the app. */
static int check_pointer_image(const zzapp_as_t *as, const aar_pointer *image)
{
    uintptr_t pixels = (uintptr_t)image->argb;

    if (!pixels || !image->width || image->width > AAR_POINTER_MAX_DIM ||
        !image->height || image->height > AAR_POINTER_MAX_DIM ||
        (uint32_t)image->hot_x >= image->width ||
        (uint32_t)image->hot_y >= image->height)
        return AAR_E_INVAL;
    if ((pixels & (sizeof(uint32_t) - 1U)) != 0U)
        return AAR_E_FAULT;
    return zzapp_svc_check_user(as, (uint32_t)pixels,
                                image->width * image->height * sizeof(uint32_t), 0);
}

/* heap_shrink's out pointer must not name memory the call gives back: its
 * store would land in a frame that is gone. */
static int heap_shrink_out_overlaps(const zzapp_proc_t *proc,
                                    const uint32_t args[SVC_MAX_WORDS],
                                    const uint32_t originals[3])
{
    uint64_t start;
    uint64_t end;
    uint64_t out;

    if (!proc || args[1] != 0U || args[0] == 0U ||
        proc->heap_break < proc->heap_base ||
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
static int call_gl_float(const aar_gl *gl, uint32_t slot, const zzapp_svc_frame_t *frame,
                         const uint32_t args[SVC_MAX_WORDS], uint32_t *result)
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
    else if (slot == GL_SLOT(ClearDepth)) gl->ClearDepth(double_argument(frame, 0U));
    else if (slot == GL_SLOT(PolygonOffset)) gl->PolygonOffset(a, b);
    else if (slot == GL_SLOT(MultiTexCoord2fARB)) gl->MultiTexCoord2fARB(args[0], a, b);
    else if (slot == GL_SLOT(Normal3f)) gl->Normal3f(a, b, c);
    else if (slot == GL_SLOT(Lightf)) gl->Lightf(args[0], args[1], a);
    else if (slot == GL_SLOT(LightModelf)) gl->LightModelf(args[0], a);
    else if (slot == GL_SLOT(Materialf)) gl->Materialf(args[0], args[1], a);
    else if (slot == GL_SLOT(Fogf)) gl->Fogf(args[0], a);
    else if (slot == GL_SLOT(LineWidth)) gl->LineWidth(a);
    else if (slot == GL_SLOT(PointSize)) gl->PointSize(a);
    else return 0;
    *result = 0U;
    return 1;
}

/* Hands the app the GL table the module returned, copied into its page with
 * a veneer for every entry. */
static int dispatch_bind(svc_state_t *st, uint32_t pid, const aar_gl *gl,
                         uint32_t *result)
{
    zzapp_proc_t *proc = zzapp_proc_slot(pid);
    struct user_gl_table *table = user_gl_of(pid);

    if (!gl || !proc || !table ||
        zzapp_svc_copy_gl_for_slot(st, &table->table, gl) != AAR_OK) {
        st->direct_gl = NULL;
        gl_client_reset(st);
        *result = 0U;
        return AAR_OK;
    }
    gl_client_reset(st);
    st->gl_client.generation = proc->generation;
    st->direct_gl = gl;
    *result = (uint32_t)(st->gl_alias ? (uintptr_t)st->gl_alias :
                         (uintptr_t)&table->table);
    return AAR_OK;
}
static int dispatch_api(const aar_api *api, svc_state_t *st, uint32_t pid,
                        zzapp_svc_frame_t *frame,
                        const zzapp_svc_descriptor_t *descriptor, uint32_t number,
                        uint32_t args[SVC_MAX_WORDS], uint32_t *result)
{
    uintptr_t entry;
    if (number == API_SLOT(gl_bind)) {
        if (!AAR_API_COVERS(api, gl_bind) || !api->gl_bind) return AAR_E_NOSYS;
        return dispatch_bind(st, pid, api->gl_bind(args[0]), result);
    }
    if (number == API_SLOT(ext) || number == ZZAPP_SVC_EXT_GL_BIND_FLAGS) {
        const aar_ext_zz9000 *runtime;

        if (!AAR_API_COVERS(api, ext) || !api->ext) return AAR_E_NOSYS;
        runtime = (const aar_ext_zz9000 *)api->ext(AAR_PLATFORM_ZZ9000,
                                                    AAR_EXT_ZZ9000_MAIN);
        if (number == API_SLOT(ext)) {
            /* The runtime's ext decides which (platform, id) pairs exist;
             * the app gets its own copy of the table. */
            const aar_ext_zz9000 *asked = (const aar_ext_zz9000 *)api->ext(args[0], args[1]);
            const struct user_api_table *table = user_api_of(pid);

            *result = asked && table && table->ext.size ?
                      (uint32_t)ext_user_address(st, table) : 0U;
            return AAR_OK;
        }
        if (!AAR_EXT_ZZ9000_COVERS(runtime, gl_bind_flags) || !runtime->gl_bind_flags)
            return AAR_E_NOSYS;
        return dispatch_bind(st, pid, runtime->gl_bind_flags(args[0], args[1]), result);
    }
    if (entry_at(api, api->size, API_FIRST, number, &entry) != AAR_OK)
        return AAR_E_NOSYS;
    if (descriptor->result == ZZAPP_SVC_RETURN_U64) {
        uint64_t value = ((uint64_t (*)(void))entry)();

        *result = (uint32_t)value;
        frame->r[1] = (uint32_t)(value >> 32);
        return AAR_OK;
    }
    *result = call_words(entry, args);
    if (number == API_SLOT(gl_unbind))
        gl_release(st, st->gl_client.generation);
    return AAR_OK;
}
static int dispatch_gl(svc_state_t *st, uint32_t pid, const zzapp_svc_frame_t *frame,
                       uint32_t number, const uint32_t args[SVC_MAX_WORDS], uint32_t *result)
{
    zzapp_proc_t *proc = zzapp_proc_slot(pid);
    struct user_gl_table *table = user_gl_of(pid);
    const aar_gl *gl = st->direct_gl;
    uintptr_t entry;
    int status = AAR_OK;

    if (!gl || !proc || !table || st->gl_client.generation != proc->generation) {
        status = AAR_E_NOSYS;
        goto done;
    }
    if (entry_at(gl, gl->size, GL_FIRST, number, &entry) != AAR_OK) {
        status = AAR_E_NOSYS;
        goto done;
    }
    if (number == GL_SLOT(GetError) && st->gl_client.error) {
        *result = st->gl_client.error;
        st->gl_client.error = 0U;
        goto done;
    }
    if (number == GL_SLOT(LockArrays) || number == GL_SLOT(UnlockArrays)) {
        gl_client_state_t *state = &st->gl_client;
        int has_is_enabled = gl->size >=
            offsetof(aar_gl, IsEnabled) + sizeof(gl->IsEnabled) &&
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
        uint32_t index = args[0] - GL_STRING_FIRST;

        if (index >= GL_STRING_COUNT) {
            *result = 0U;
            goto done;
        }
        if (!(st->gl_client.strings_cached & (1U << index))) {
            const uint8_t *string = gl->GetString(args[0]);

            if (!string) {
                *result = 0U;
                goto done;
            }
            copy_table_string(table->strings[index], sizeof(table->strings[index]),
                              (const char *)string);
            st->gl_client.strings_cached |= 1U << index;
        }
        *result = st->gl_alias ? st->gl_alias +
            (uint32_t)(table->strings[index] - (char *)table) :
            (uint32_t)(uintptr_t)table->strings[index];
        goto done;
    }
    if (call_gl_float(gl, number, frame, args, result))
        goto done;
    *result = call_words(entry, args);
    gl_record_client_call(st, number, args);
done:
    gl_telemetry_frontend_end(st);
    return status;
}

static void zzapp_svc_dispatch_slow(const aar_api *api, zzapp_proc_t *proc,
                                    svc_state_t *st, uint32_t pid,
                                    uint32_t number, zzapp_svc_frame_t *frame)
{
    const zzapp_svc_descriptor_t *descriptor;
    uint32_t result = 0U;
    uint32_t args[SVC_MAX_WORDS] = { 0U };
    _Alignas(8) uint8_t copies[3][COPY_BYTES];
    char strings[3][STRING_BYTES];
    uint32_t originals[3] = { 0U };
    uint32_t lengths[3] = { 0U };
    uint32_t tails[3] = { 0U };
    int gl;
    int status;

    gl = number >= ZZAPP_SVC_GL_FIRST;
    number -= gl ? ZZAPP_SVC_GL_FIRST : 0U;
    if (gl)
        gl_telemetry_frame_begin(st, number);
    descriptor = gl ? zzapp_svc_gl_descriptor(number) :
                      zzapp_svc_api_descriptor(number);
    if (!descriptor) {
        if (gl) {
            gl_telemetry_descriptor_failure(st);
            gl_telemetry_descriptor_end(st);
        }
        frame->r[0] = (uint32_t)AAR_E_NOSYS;
        return;
    }
    if (gl)
        counters_handler_start(st);
    status = frame_arguments(&proc->as, frame, descriptor->arguments, args);
    if (!gl && number == API_SLOT(display_pointer) &&
        args[1] != AAR_POINTER_CUSTOM)
        args[2] = 0U;
    if (status == AAR_OK)
        status = prepare_pointers(st, &proc->as, descriptor, args, copies,
                                  strings, originals, lengths, tails);
    if (status != AAR_OK) {
        if (gl) {
            gl_telemetry_pointer_failure(st);
            gl_telemetry_descriptor_end(st);
            counters_handler_end(st);
        }
        frame->r[0] = (uint32_t)status;
        return;
    }
    if (gl && (number == GL_SLOT(DrawArrays) ||
               number == GL_SLOT(DrawElements)) &&
        !gl_validate_draw(&proc->as, st, number, args)) {
        if (!st->gl_client.error)
            st->gl_client.error = GL_INVALID_VALUE;
        gl_telemetry_pointer_failure(st);
        gl_telemetry_descriptor_end(st);
        counters_handler_end(st);
        frame->r[0] = (uint32_t)AAR_E_FAULT;
        return;
    }
    if (!gl && number == API_SLOT(heap_shrink) &&
        heap_shrink_out_overlaps(proc, args, originals)) {
        frame->r[0] = (uint32_t)AAR_E_FAULT;
        return;
    }
    if (!gl && number == API_SLOT(image_open) && originals[0]) {
        const aar_image_params *open =
            (const aar_image_params *)(const void *)copies[0];

        if (zzapp_svc_check_user(&proc->as, (uint32_t)(uintptr_t)open->tile,
                                 open->tile_bytes, 1) != AAR_OK) {
            frame->r[0] = (uint32_t)AAR_E_FAULT;
            return;
        }
    }
    if (!gl && number == API_SLOT(display_pointer) &&
        args[1] == AAR_POINTER_CUSTOM && originals[0]) {
        status = check_pointer_image(&proc->as,
            (const aar_pointer *)(const void *)copies[0]);
        if (status != AAR_OK) {
            frame->r[0] = (uint32_t)status;
            return;
        }
    }
    if (gl) {
        gl_telemetry_descriptor_end(st);
        gl_telemetry_frontend_begin(st);
    } else
        counters_handler_start(st);
    status = gl ? dispatch_gl(st, pid, frame, number, args, &result) :
                  dispatch_api(api, st, pid, frame, descriptor, number, args,
                               &result);
    counters_handler_end(st);
    if (status != AAR_OK) {
        frame->r[0] = (uint32_t)(status == AAR_E_FAULT ? AAR_E_FAULT : AAR_E_NOSYS);
        return;
    }
    finish_pointers(descriptor, copies, originals, lengths, tails, result);
    if (descriptor->result != ZZAPP_SVC_RETURN_VOID)
        frame->r[0] = result;
}

static int dispatch_fast_no_lock(const aar_api *api, uint32_t number,
                                 zzapp_svc_frame_t *frame)
{
    if (number == API_SLOT(should_stop)) {
        if (!api->should_stop)
            return AAR_E_NOSYS;
        frame->r[0] = (uint32_t)api->should_stop();
        return AAR_OK;
    }
    if (number == API_SLOT(time_us)) {
        uint64_t value;

        if (!api->time_us)
            return AAR_E_NOSYS;
        value = api->time_us();
        frame->r[0] = (uint32_t)value;
        frame->r[1] = (uint32_t)(value >> 32);
        return AAR_OK;
    }
    return AAR_E_NOSYS;
}

/* The record a call from the running thread belongs to: the scheduled
 * thread's, else the one the services layer is serving. ZZAPP_PID_NONE when
 * there is neither; the caller then fails the call. */
static uint32_t svc_caller(void)
{
    int thread = zzapp_kernel_current_thread();

    return thread >= 0 ? (uint32_t)thread : zzapp_services_current_pid();
}

#if ZZAPP_SVC_PROBES && ZZAPP_PERF_COUNTERS
static void probe_invalidate_counter_tail(void)
{
    svc_state_t *st = svc_state(svc_caller());

    if (st)
        counters_selected(st)->handler_started = 0U;
}
#endif

#if !ZZAPP_SVC_PROBES
void zzapp_svc_dispatch(zzapp_svc_frame_t *frame)
{
    const aar_api *api;
    zzapp_proc_t *proc;
    const zzapp_svc_descriptor_t *descriptor;
    svc_state_t *st;
    uint32_t number;
    uint32_t pid;
    int status;

    if (!frame)
        return;
    number = frame->r[12];
    if (number < API_COUNT) {
        descriptor = &api_descriptors[number];
        if (descriptor->flags & ZZAPP_SVC_FAST_NO_LOCK) {
            st = svc_state(svc_caller());
            if (!st) {
                frame->r[0] = (uint32_t)AAR_E_FAULT;
                return;
            }
            counters_dispatch(st, 0);
            api = zzapp_api_table();
            if (!api) {
                frame->r[0] = (uint32_t)AAR_E_FAULT;
                return;
            }
            st->last_number = number;
            counters_handler_start(st);
            status = dispatch_fast_no_lock(api, number, frame);
            counters_handler_end(st);
            if (status != AAR_OK)
                frame->r[0] = (uint32_t)AAR_E_NOSYS;
            return;
        }
    }
    pid = zzapp_services_current_pid();
    st = svc_state(pid);
    if (!st) {
        frame->r[0] = (uint32_t)AAR_E_FAULT;
        return;
    }
    counters_dispatch(st, number >= ZZAPP_SVC_GL_FIRST);
    api = zzapp_api_table();
    proc = zzapp_proc_slot(pid);
    if (!proc || !api) {
        frame->r[0] = (uint32_t)AAR_E_FAULT;
        return;
    }
    st->last_number = number;
    zzapp_svc_dispatch_slow(api, proc, st, pid, number, frame);
}
#else
void zzapp_svc_dispatch(zzapp_svc_frame_t *frame)
{
    const aar_api *api;
    const zzapp_svc_descriptor_t *descriptor;
    svc_state_t *st;
    uint32_t number;
    uint32_t pid;
    int status;
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

        pid = zzapp_services_current_pid();
        st = svc_state(pid);
        if (!st) {
            frame->r[0] = (uint32_t)AAR_E_FAULT;
            return;
        }
        counters_dispatch(st, number >= ZZAPP_SVC_GL_FIRST);
        api = zzapp_api_table();
        proc = zzapp_proc_slot(pid);
        if (!proc || !api) {
            frame->r[0] = (uint32_t)AAR_E_FAULT;
            return;
        }
        st->last_number = number;
        zzapp_svc_dispatch_slow(api, proc, st, pid, number, frame);
        return;
    }
    st = svc_state(svc_caller());
    if (!st) {
        frame->r[0] = (uint32_t)AAR_E_FAULT;
        return;
    }
    counters_dispatch(st, 0);
    api = zzapp_api_table();
    if (!api) {
        frame->r[0] = (uint32_t)AAR_E_FAULT;
        return;
    }
    st->last_number = number;
    counters_handler_start(st);
    if (number == ZZAPP_SVC_PROBE_P4) {
        frame->r[0] = 0U;
        status = AAR_OK;
    } else
        status = dispatch_fast_no_lock(api, number, frame);
    counters_handler_end(st);
    if (status != AAR_OK)
        frame->r[0] = (uint32_t)AAR_E_NOSYS;
}
#endif
