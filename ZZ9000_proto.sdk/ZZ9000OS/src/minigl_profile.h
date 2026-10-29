/*
 * MiniGL wire-v9 pipeline stage profiler.
 *
 * Exclusive-time stage accounting for the packet path on the MiniGL worker
 * core: a region pushes its stage, and every timer read charges the time
 * since the previous read to the stage on top of the stack, so nested
 * regions (a fence wait inside a flush inside a draw) never double count.
 * Depth 0 is IDLE: the worker loop outside any MiniGL work.
 *
 * Off by default. The host turns it on by writing a non-zero
 * ZZ9KMiniGLStatusV9.trace_enabled (the profiling minigl.library does that
 * when MGL_PROFILE=1); the service samples that field once per poll, at
 * stack depth 0. While off, every hook is one load and a not-taken branch.
 * While on, one summary line goes to the UART every 5 s or more, and only
 * if MiniGL work happened in the window.
 *
 * Host test builds compile every hook to nothing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MINIGL_PROFILE_H
#define MINIGL_PROFILE_H

#include <stdint.h>

typedef enum {
    MGLP_IDLE = 0,  /* worker outside MiniGL work (loop, scheduler, spin) */
    MGLP_PARSE,     /* packet validate, command walk, decode, state exec */
    MGLP_GEOM,      /* vertex convert, clip, RasterIX triangle setup */
    MGLP_CPU,       /* software rasterizer draws (fallback) */
    MGLP_EMIT,      /* RasterIX command buffer build */
    MGLP_FIFO,      /* command buffer clean + FIFO push (MMIO/PL330) */
    MGLP_STALL,     /* FIFO vacancy (TDFV) waits */
    MGLP_FENCE,     /* RasterIX fence and idle waits */
    MGLP_TARGET,    /* RGB565/Z16 <-> RGBA8/Z24 target conversion */
    MGLP_TEX,       /* texture image/sub-image/copy upload */
    MGLP_PRESENT,   /* synchronous BLIT/CAPTURE op */
    MGLP_OP,        /* other synchronous ops (create, readback, request) */
    MGLP_STAGES
} MiniGLProfileStage;

typedef enum {
    MGLP_N_TRIANGLES = 0, /* triangles queued for RasterIX */
    MGLP_N_CULLED,        /* triangles RasterIX setup found empty */
    MGLP_N_DRAW_HW,       /* draws completed on RasterIX */
    MGLP_N_DRAW_SW,       /* draws rasterized on the CPU */
    MGLP_N_FIFO_WORDS,    /* command words pushed */
    MGLP_N_FIFO_SUBMITS,  /* command buffer submissions */
    MGLP_N_TEX_UPLOADS,   /* texture upload commands */
    MGLP_N_RIX_UPLOADS,   /* texture levels written to RasterIX memory */
    MGLP_N_PIN_DRAINS,    /* uploads that drained RasterIX (texture in use) */
    MGLP_N_EVICTIONS,     /* textures evicted from the RasterIX pool */
    MGLP_N_OPS,           /* synchronous bridge ops */
    MGLP_N_ASYNC_PRESENTS,/* packet-owned direct P96 presentations */
    MGLP_COUNTERS
} MiniGLProfileCounter;

#if defined(__arm__) && !defined(MINIGL_HOST_TEST) && \
    !defined(MINIGL_SERVICE_HOST_TEST)
#define MINIGL_PROFILE_ACTIVE 1

extern volatile int g_minigl_profile_on;
extern uint32_t g_minigl_profile_counts[MGLP_COUNTERS];

void minigl_profile_push_on(MiniGLProfileStage stage);
void minigl_profile_pop_on(void);

static inline void minigl_profile_push(MiniGLProfileStage stage)
{
    if (g_minigl_profile_on)
        minigl_profile_push_on(stage);
}

static inline void minigl_profile_pop(void)
{
    if (g_minigl_profile_on)
        minigl_profile_pop_on();
}

static inline void minigl_profile_count(MiniGLProfileCounter counter,
                                        uint32_t amount)
{
    if (g_minigl_profile_on)
        g_minigl_profile_counts[counter] += amount;
}

/* Service poll entry, stack depth 0 only: follow the host's switch. */
void minigl_profile_set_enabled(int enabled);
/* One ring packet: PARSE envelope, per-packet maximum, byte counters. */
void minigl_profile_packet_begin(void);
void minigl_profile_packet_end(uint32_t bytes, uint32_t commands);
/* First CPU-fallback reason seen in the window (0 = none). */
void minigl_profile_fallback(uint32_t reason);
/* Service poll exit: prints the summary once 5 s have elapsed. */
void minigl_profile_report(void);
#else
static inline void minigl_profile_push(MiniGLProfileStage stage) { (void)stage; }
static inline void minigl_profile_pop(void) { }
static inline void minigl_profile_count(MiniGLProfileCounter counter,
                                        uint32_t amount)
{
    (void)counter;
    (void)amount;
}
static inline void minigl_profile_set_enabled(int enabled) { (void)enabled; }
static inline void minigl_profile_packet_begin(void) { }
static inline void minigl_profile_packet_end(uint32_t bytes, uint32_t commands)
{
    (void)bytes;
    (void)commands;
}
static inline void minigl_profile_fallback(uint32_t reason) { (void)reason; }
static inline void minigl_profile_report(void) { }
#endif

#endif /* MINIGL_PROFILE_H */
