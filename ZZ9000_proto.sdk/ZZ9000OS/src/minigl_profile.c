/*
 * MiniGL wire-v9 pipeline stage profiler (see minigl_profile.h).
 *
 * Clock: the service-owned monotonic microsecond clock. Single writer: the
 * core running mw_service_poll (the MiniGL worker, or core 0 when no worker
 * exists; never both at once).
 *
 * The rate-limited summary runs between windows: its own duration is reported
 * in the next line as log= and never lands in a stage.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "minigl_profile.h"
#include "minigl_platform.h"
#if defined(MINIGL_PROFILE_ACTIVE)
#include <stdio.h>
#include <string.h>

#define MGLP_WINDOW_TICKS (5ULL * 1000000ULL)

volatile int g_minigl_profile_on;
uint32_t g_minigl_profile_counts[MGLP_COUNTERS];

static struct {
    uint64_t ticks[MGLP_STAGES];
    uint64_t mark;
    uint64_t window_start;
    uint64_t packet_busy_start;
    uint64_t packet_max;
    uint64_t last_log;
    uint32_t depth;
    uint32_t overflow;
    uint32_t packets;
    uint32_t bytes;
    uint32_t commands;
    uint32_t fallback_reason;
    uint32_t fallback_draws[ZZ9K_MGL_FALLBACK_MAX];
    uint32_t fallback_triangles[ZZ9K_MGL_FALLBACK_MAX];
    uint8_t stack[MGLP_STACK_DEPTH];
} g_mglp;

static uint64_t mglp_now(void)
{
    return minigl_platform_time_us();
}

static void mglp_charge(void)
{
    uint64_t now = mglp_now();
    MiniGLProfileStage stage = g_mglp.depth != 0U ?
        (MiniGLProfileStage)g_mglp.stack[g_mglp.depth - 1U] : MGLP_IDLE;

    g_mglp.ticks[stage] += now - g_mglp.mark;
    g_mglp.mark = now;
}

static uint64_t mglp_packet_busy(void)
{
    unsigned int stage;
    uint64_t busy = 0U;

    for (stage = MGLP_PARSE; stage <= MGLP_TEX; ++stage)
        busy += g_mglp.ticks[stage];
    return busy;
}

static void mglp_window_reset(void)
{
    memset(g_mglp.ticks, 0, sizeof(g_mglp.ticks));
    memset(g_minigl_profile_counts, 0, sizeof(g_minigl_profile_counts));
    g_mglp.packet_max = 0U;
    g_mglp.packets = 0U;
    g_mglp.bytes = 0U;
    g_mglp.commands = 0U;
    g_mglp.fallback_reason = ZZ9K_MGL_FALLBACK_NONE;
    memset(g_mglp.fallback_draws, 0, sizeof(g_mglp.fallback_draws));
    memset(g_mglp.fallback_triangles, 0, sizeof(g_mglp.fallback_triangles));
    g_mglp.mark = mglp_now();
    g_mglp.window_start = g_mglp.mark;
}

void minigl_profile_push_on(MiniGLProfileStage stage)
{
    mglp_charge();
    if (g_mglp.depth < MGLP_STACK_DEPTH)
        g_mglp.stack[g_mglp.depth++] = (uint8_t)stage;
    else
        ++g_mglp.overflow;
}

void minigl_profile_pop_on(void)
{
    mglp_charge();
    if (g_mglp.overflow != 0U)
        --g_mglp.overflow;
    else if (g_mglp.depth != 0U)
        --g_mglp.depth;
}

void minigl_profile_set_enabled(int enabled)
{
    if (enabled && !g_minigl_profile_on) {
        g_mglp.depth = 0U;
        g_mglp.overflow = 0U;
        g_mglp.last_log = 0U;
        mglp_window_reset();
        g_minigl_profile_on = 1;
    } else if (!enabled && g_minigl_profile_on) {
        g_minigl_profile_on = 0;
    }
}

void minigl_profile_packet_begin(void)
{
    if (!g_minigl_profile_on)
        return;
    minigl_profile_push_on(MGLP_PARSE);
    g_mglp.packet_busy_start = mglp_packet_busy();
}

void minigl_profile_packet_end(uint32_t bytes, uint32_t commands)
{
    uint64_t busy;

    if (!g_minigl_profile_on)
        return;
    minigl_profile_pop_on();
    busy = mglp_packet_busy() - g_mglp.packet_busy_start;
    if (busy > g_mglp.packet_max)
        g_mglp.packet_max = busy;
    ++g_mglp.packets;
    g_mglp.bytes += bytes;
    g_mglp.commands += commands;
}

void minigl_profile_fallback(uint32_t reason, uint32_t triangles)
{
    if (!g_minigl_profile_on || reason >= ZZ9K_MGL_FALLBACK_MAX)
        return;
    if (g_mglp.fallback_reason == ZZ9K_MGL_FALLBACK_NONE)
        g_mglp.fallback_reason = reason;
    ++g_mglp.fallback_draws[reason];
    g_mglp.fallback_triangles[reason] += triangles;
}

/* Tenths of a millisecond, divided over count events (0 = total). */
static uint32_t mglp_tenth_ms(uint64_t ticks, uint32_t count)
{
    uint64_t value = ticks / 100U;

    if (count != 0U)
        value /= count;
    return value > 0xffffffffULL ? 0xffffffffU : (uint32_t)value;
}

static size_t mglp_append_ms(char *line, size_t used, size_t size,
                             const char *prefix, uint64_t ticks, uint32_t count)
{
    uint32_t value = mglp_tenth_ms(ticks, count);
    int written;

    if (used >= size)
        return used;
    written = snprintf(line + used, size - used, "%s%lu.%lu", prefix,
                       (unsigned long)(value / 10U), (unsigned long)(value % 10U));
    return written > 0 ? used + (size_t)written : used;
}

void minigl_profile_report(void)
{
    static const char *const names[MGLP_STAGES] = {
        " idle=", " parse=", " geom=", " cpu=", " emit=", " fifo=", " stall=",
        " fence=", " tgt=", " tex=", " pres=", " op="
    };
    static char line[768];
    const uint32_t *n = g_minigl_profile_counts;
    uint64_t print_start;
    uint32_t window;
    size_t used;
    int written;
    unsigned int stage;

    unsigned int reason;
    if (!g_minigl_profile_on || g_mglp.depth != 0U || g_mglp.overflow != 0U ||
        mglp_now() - g_mglp.window_start < MGLP_WINDOW_TICKS)
        return;
    mglp_charge();
    if (g_mglp.packets == 0U && n[MGLP_N_OPS] == 0U) {
        mglp_window_reset();
        return;
    }
    print_start = g_mglp.mark;
    window = mglp_tenth_ms(g_mglp.mark - g_mglp.window_start, 0U) / 1000U;
    written = snprintf(line, sizeof(line),
                       "[MGLP] %lu.%lus pk=%lu op=%lu ap=%lu kB=%lu cmd=%lu tri=%lu "
                       "cull=%lu hw=%lu sw=%lu fb=%lu fw=%lu sub=%lu tx=%lu "
                       "up=%lu ub=%lu pin=%lu ev=%lu fbk=",
                       (unsigned long)(window / 10U), (unsigned long)(window % 10U),
                       (unsigned long)g_mglp.packets, (unsigned long)n[MGLP_N_OPS],
                       (unsigned long)n[MGLP_N_ASYNC_PRESENTS],
                       (unsigned long)(g_mglp.bytes / 1024U),
                       (unsigned long)g_mglp.commands,
                       (unsigned long)n[MGLP_N_TRIANGLES],
                       (unsigned long)n[MGLP_N_CULLED],
                       (unsigned long)n[MGLP_N_DRAW_HW],
                       (unsigned long)n[MGLP_N_DRAW_SW],
                       (unsigned long)g_mglp.fallback_reason,
                       (unsigned long)n[MGLP_N_FIFO_WORDS],
                       (unsigned long)n[MGLP_N_FIFO_SUBMITS],
                       (unsigned long)n[MGLP_N_TEX_UPLOADS],
                       (unsigned long)n[MGLP_N_RIX_UPLOADS],
                       (unsigned long)(n[MGLP_N_RIX_UPLOAD_BYTES] / 1024U),
                       (unsigned long)n[MGLP_N_PIN_DRAINS],
                       (unsigned long)n[MGLP_N_EVICTIONS]);
    used = written > 0 ? (size_t)written : 0U;
    for (reason = 1U; reason < ZZ9K_MGL_FALLBACK_MAX; ++reason)
        if (g_mglp.fallback_draws[reason] != 0U) {
            written = snprintf(line + used, sizeof(line) - used, "%u:%lu/%lu,",
                               reason, (unsigned long)g_mglp.fallback_draws[reason],
                               (unsigned long)g_mglp.fallback_triangles[reason]);
            if (written <= 0 || (size_t)written >= sizeof(line) - used)
                break;
            used += (size_t)written;
        }
    if (used != 0U && line[used - 1U] == ',')
        --used;
    written = snprintf(line + used, sizeof(line) - used, " |");
    if (written > 0 && (size_t)written < sizeof(line) - used)
        used += (size_t)written;
    /* Stage columns are milliseconds per ring packet (one GLQuake frame);
     * an op-only window prints window totals instead. */
    for (stage = MGLP_PARSE; stage < MGLP_STAGES; ++stage)
        used = mglp_append_ms(line, used, sizeof(line), names[stage],
                              g_mglp.ticks[stage], g_mglp.packets);
    used = mglp_append_ms(line, used, sizeof(line), names[MGLP_IDLE],
                          g_mglp.ticks[MGLP_IDLE], g_mglp.packets);
    used = mglp_append_ms(line, used, sizeof(line), " | max=",
                          g_mglp.packet_max, 0U);
    /* One rate-limited line: core 0 and the worker share the UART. */
    minigl_platform_log(line);
    mglp_window_reset();
    g_mglp.last_log = g_mglp.mark - print_start;
}
#endif
