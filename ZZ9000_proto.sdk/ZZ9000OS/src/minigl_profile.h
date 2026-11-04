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
    MGLP_SWAP_DRAIN,/* waiting for a queued RasterIX frame */
    MGLP_SWAP_COPY, /* colour target copy/conversion into a P96 bitmap */
    MGLP_SWAP_FLIP, /* waiting for a P96 flip slot (or publishing a flip) */
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
    MGLP_N_RIX_UPLOADS,   /* texture levels/rectangles written to RasterIX */
    MGLP_N_RIX_UPLOAD_BYTES, /* bytes written to RasterIX texture pages */
    MGLP_N_PIN_DRAINS,    /* uploads that drained RasterIX (texture in use) */
    MGLP_N_EVICTIONS,     /* textures evicted from the RasterIX pool */
    MGLP_N_OPS,           /* synchronous bridge ops */
    MGLP_N_FRAMES,        /* MW_PRESENT swaps: the per-frame divisor */
    MGLP_N_ASYNC_PRESENTS,/* packet-owned direct P96 presentations */
    MGLP_N_DRV_PACKETS,  /* FIFO packets pushed to RasterIX (MMIO or DMA) */
    /* RasterIX state sampled once per FIFO packet, before it is pushed: */
    MGLP_N_HW_SAMPLES,    /* samples taken */
    MGLP_N_HW_BUSY,       /* renderer busy */
    MGLP_N_HW_STALL,      /* renderer pipeline stalled */
    MGLP_N_HW_READS,      /* sum of outstanding AXI reads */
    MGLP_N_HW_VACANCY,    /* sum of free FIFO words (TDFV) */
    MGLP_N_RIX_STATES,    /* RasterIX state blocks emitted (per draw op/pass) */
    MGLP_N_RIX_PAGE_WORDS,/* texture page-list words in those blocks */
    MGLP_COUNTERS
} MiniGLProfileCounter;

typedef enum {
    MGLP_GEOM_CONV = 0,   /* vertex convert and transform (v9_vert_convert) */
    MGLP_GEOM_CLIP,       /* polygon clip, project, and screen setup */
    MGLP_GEOM_SETUP,      /* RasterIX triangle setup (rix_setup_triangle) */
    MGLP_GEOM_REC,        /* descriptor record / buffer copy */
    MGLP_GEOM_SUB_STAGES
} MiniGLProfileGeomSubStage;

#if defined(__arm__) && !defined(MINIGL_HOST_TEST) && \
    !defined(MINIGL_SERVICE_HOST_TEST)
#define MINIGL_PROFILE_ACTIVE 1

extern volatile int g_minigl_profile_on;
extern uint32_t g_minigl_profile_counts[MGLP_COUNTERS];
extern uint64_t g_minigl_profile_geom_cycles[MGLP_GEOM_SUB_STAGES];
extern uint64_t g_minigl_profile_geom_total_cycles;

static inline uint32_t minigl_profile_cycles(void)
{
    uint32_t c;
    __asm__ __volatile__("mrc p15, 0, %0, c9, c13, 0" : "=r"(c));
    return c;
}

static inline void minigl_profile_geom_add_cycles(MiniGLProfileGeomSubStage sub, uint32_t start_cycles)
{
    if (g_minigl_profile_on) {
        uint32_t delta = minigl_profile_cycles() - start_cycles;
        g_minigl_profile_geom_cycles[sub] += (uint64_t)delta;
    }
}

static inline void minigl_profile_geom_add_total_cycles(uint32_t start_cycles)
{
    if (g_minigl_profile_on) {
        uint32_t delta = minigl_profile_cycles() - start_cycles;
        g_minigl_profile_geom_total_cycles += (uint64_t)delta;
    }
}

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

static inline int minigl_profile_enabled(void)
{
    return g_minigl_profile_on;
}

/* Service poll entry, stack depth 0 only: follow the host's switch. */
void minigl_profile_set_enabled(int enabled);
/* One ring packet: PARSE envelope, per-packet maximum, byte counters. */
void minigl_profile_packet_begin(void);
void minigl_profile_packet_end(uint32_t bytes, uint32_t commands);
/* Counts CPU draws and triangles by their normalized fallback reason. */
void minigl_profile_fallback(uint32_t reason, uint32_t triangles);
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
static inline int minigl_profile_enabled(void) { return 0; }
static inline uint32_t minigl_profile_cycles(void) { return 0U; }
static inline void minigl_profile_geom_add_cycles(MiniGLProfileGeomSubStage sub, uint32_t start_cycles)
{
    (void)sub;
    (void)start_cycles;
}
static inline void minigl_profile_geom_add_total_cycles(uint32_t start_cycles)
{
    (void)start_cycles;
}
static inline void minigl_profile_set_enabled(int enabled) { (void)enabled; }
static inline void minigl_profile_packet_begin(void) { }
static inline void minigl_profile_packet_end(uint32_t bytes, uint32_t commands)
{
    (void)bytes;
    (void)commands;
}
static inline void minigl_profile_fallback(uint32_t reason, uint32_t triangles)
{
    (void)reason;
    (void)triangles;
}
static inline void minigl_profile_report(void) { }
#endif

#endif /* MINIGL_PROFILE_H */
