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
#include "minigl_renderer.h"

#include "minigl_platform.h"
#include "rix_drv.h"
#if defined(MINIGL_PROFILE_ACTIVE)
#include <stdio.h>
#include <string.h>

#include "minigl_raster.h"

#define MGLP_STACK_DEPTH 16U
#define MGLP_WINDOW_TICKS (5ULL * 1000000ULL)

volatile int g_minigl_profile_on;
uint32_t g_minigl_profile_counts[MGLP_COUNTERS];
uint64_t g_minigl_profile_geom_cycles[MGLP_GEOM_SUB_STAGES];
uint64_t g_minigl_profile_geom_total_cycles;

static struct {
    uint64_t ticks[MGLP_STAGES];
    uint64_t mark;
    uint64_t window_start;
    uint64_t packet_busy_start;
    uint64_t packet_max;
    uint64_t last_log;
    uint32_t busy_start;
    uint32_t feed_chunks[RIX_DRV_FEED_SOURCE_COUNT];
    uint32_t feed_empty;
    uint32_t feed_full;
    uint32_t feed_retries;
    uint32_t feed_tse;
    uint32_t feed_tpoe;
    uint32_t feed_dma_complete;
    uint32_t feed_deadline_wakeups;
    uint32_t feed_waits[MGLP_FEED_WAIT_COUNT];
    uint64_t feed_wait_us[MGLP_FEED_WAIT_COUNT];
    uint32_t queued_high_water;
    uint32_t depth;
    uint32_t overflow;
    uint32_t packets;
    uint32_t bytes;
    uint32_t commands;
    uint32_t fallback_reason;
    uint32_t fallback_draws[ZZ9K_MGL_FALLBACK_MAX];
    uint32_t fallback_triangles[ZZ9K_MGL_FALLBACK_MAX];
    int busy_valid;
    uint8_t stack[MGLP_STACK_DEPTH];
} g_mglp;

static uint64_t mglp_now(void)
{
    return minigl_platform_time_us();
}

static void mglp_snapshot_busy(void)
{
    uint32_t version;

    if (!minigl_platform_has_rasterix() ||
        minigl_platform_csr_read(0, RIX_DRV_CSR_IDENTITY) != RIX_DRV_IDENTITY_VALUE) {
        g_mglp.busy_valid = 0;
        return;
    }
    version = minigl_platform_csr_read(0, RIX_DRV_CSR_VERSION);
    if ((version & 0xffff0000U) != (RIX_DRV_VERSION_VALUE & 0xffff0000U) ||
        version < RIX_DRV_VERSION_BUSY_CYCLES) {
        g_mglp.busy_valid = 0;
        return;
    }
    g_mglp.busy_start = minigl_platform_csr_read(0, RIX_DRV_CSR_DBG_BUSY_CYCLES);
    g_mglp.busy_valid = 1;
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
    memset(g_minigl_profile_geom_cycles, 0, sizeof(g_minigl_profile_geom_cycles));
    memset(g_mglp.feed_chunks, 0, sizeof(g_mglp.feed_chunks));
    g_minigl_profile_geom_total_cycles = 0U;
    g_mglp.packet_max = 0U;
    g_mglp.packets = 0U;
    g_mglp.bytes = 0U;
    g_mglp.commands = 0U;
    g_mglp.feed_empty = 0U;
    g_mglp.feed_full = 0U;
    g_mglp.feed_retries = 0U;
    g_mglp.feed_tse = 0U;
    g_mglp.feed_tpoe = 0U;
    g_mglp.feed_dma_complete = 0U;
    g_mglp.queued_high_water = 0U;
    g_mglp.feed_deadline_wakeups = 0U;
    memset(g_mglp.feed_waits, 0, sizeof(g_mglp.feed_waits));
    memset(g_mglp.feed_wait_us, 0, sizeof(g_mglp.feed_wait_us));
    g_mglp.fallback_reason = ZZ9K_MGL_FALLBACK_NONE;
    memset(g_mglp.fallback_draws, 0, sizeof(g_mglp.fallback_draws));
    memset(g_mglp.fallback_triangles, 0, sizeof(g_mglp.fallback_triangles));
    g_mglp.mark = mglp_now();
    g_mglp.window_start = g_mglp.mark;
    mglp_snapshot_busy();
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
#if defined(__arm__) && !defined(MINIGL_HOST_TEST)
        {
            uint32_t pmcr;
            __asm__ __volatile__("mrc p15, 0, %0, c9, c12, 0" : "=r"(pmcr));
            pmcr |= 0x01U;  /* E = 1: enable performance counters */
            pmcr &= ~0x08U; /* D = 0: cycle counter counts every clock cycle */
            __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 0" : : "r"(pmcr));
            __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 1" : : "r"(0x80000000U)); /* enable PMCCNTR */
        }
#endif
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
#if defined(__arm__) && !defined(MINIGL_HOST_TEST)
    {
        uint32_t pmcr;
        __asm__ __volatile__("mrc p15, 0, %0, c9, c12, 0" : "=r"(pmcr));
        pmcr |= 0x01U;
        pmcr &= ~0x08U;
        __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 0" : : "r"(pmcr));
        __asm__ __volatile__("mcr p15, 0, %0, c9, c12, 1" : : "r"(0x80000000U));
    }
#endif
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

void minigl_profile_feed_chunk(uint32_t source)
{
    if (g_minigl_profile_on && source < RIX_DRV_FEED_SOURCE_COUNT)
        ++g_mglp.feed_chunks[source];
}

void minigl_profile_feed_vacancy(uint32_t vacancy, uint32_t capacity)
{
    if (!g_minigl_profile_on)
        return;
    if (capacity != 0U && vacancy == capacity)
        ++g_mglp.feed_empty;
    if (vacancy == 0U)
        ++g_mglp.feed_full;
}

void minigl_profile_feed_retry(void)
{
    if (g_minigl_profile_on)
        ++g_mglp.feed_retries;
}

void minigl_profile_feed_errors(uint32_t errors)
{
    if (!g_minigl_profile_on)
        return;
    if ((errors & RIX_DRV_FIFO_ISR_TSE) != 0U)
        ++g_mglp.feed_tse;
    if ((errors & RIX_DRV_FIFO_ISR_TPOE) != 0U)
        ++g_mglp.feed_tpoe;
}

void minigl_profile_feed_dma_complete(void)
{
    if (g_minigl_profile_on)
        ++g_mglp.feed_dma_complete;
}

void minigl_profile_feed_queued(uint32_t queued)
{
    if (g_minigl_profile_on && queued > g_mglp.queued_high_water)
        g_mglp.queued_high_water = queued;
}

void minigl_profile_feed_deadline_wake(void)
{
    if (g_minigl_profile_on)
        ++g_mglp.feed_deadline_wakeups;
}

void minigl_profile_feed_wait(MiniGLProfileFeedWait reason, uint64_t us)
{
    if (g_minigl_profile_on && reason < MGLP_FEED_WAIT_COUNT) {
        ++g_mglp.feed_waits[reason];
        g_mglp.feed_wait_us[reason] += us;
    }
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
    uint32_t divisor;
    static const char *const names[MGLP_STAGES] = {
        " idle=", " parse=", " geom=", " cpu=", " emit=", " fifo=", " stall=",
        " fence=", " tgt=", " tex=", " pres=", " sdr=", " scpy=", " sfli=", " op="
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
    /* Stage times and per-unit hardware figures are per frame: a 68k
     * session sends about one ring packet per frame, an ARM app several
     * in-process packets, so dividing by packets understated ARM frames. */
    divisor = n[MGLP_N_FRAMES] != 0U ? n[MGLP_N_FRAMES] :
              (g_mglp.packets != 0U ? g_mglp.packets : 1U);
    print_start = g_mglp.mark;
    window = mglp_tenth_ms(g_mglp.mark - g_mglp.window_start, 0U) / 1000U;
    written = snprintf(line, sizeof(line),
                       "[MGLP] %lu.%lus fr=%lu pk=%lu op=%lu ap=%lu kB=%lu cmd=%lu tri=%lu "
                       "cull=%lu hw=%lu sw=%lu fb=%lu fw=%lu sub=%lu fp=%lu tx=%lu "
                       "up=%lu ub=%lu pin=%lu ev=%lu fbk=",
                       (unsigned long)(window / 10U), (unsigned long)(window % 10U),
                       (unsigned long)n[MGLP_N_FRAMES],
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
                       (unsigned long)n[MGLP_N_DRV_PACKETS],
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
    for (stage = MGLP_PARSE; stage < MGLP_STAGES; ++stage) {
        used = mglp_append_ms(line, used, sizeof(line), names[stage],
                              g_mglp.ticks[stage], divisor);
        if (stage == MGLP_GEOM) {
            static const char *const sub_names[MGLP_GEOM_SUB_STAGES] = {
                " g_conv=", " g_clip=", " g_setup=", " g_rec="
            };
            unsigned int sub;
            for (sub = 0U; sub < MGLP_GEOM_SUB_STAGES; ++sub) {
                uint64_t sub_ticks = 0U;
                if (g_minigl_profile_geom_total_cycles > 0U && g_mglp.ticks[MGLP_GEOM] > 0U) {
                    sub_ticks = (uint64_t)((double)g_mglp.ticks[MGLP_GEOM] *
                                (double)g_minigl_profile_geom_cycles[sub] /
                                (double)g_minigl_profile_geom_total_cycles + 0.5);
                } else if (g_minigl_profile_geom_cycles[sub] > 0U) {
                    sub_ticks = g_minigl_profile_geom_cycles[sub] / 666U;
                }
                used = mglp_append_ms(line, used, sizeof(line), sub_names[sub],
                                      sub_ticks, divisor);
            }
        }
    }
    used = mglp_append_ms(line, used, sizeof(line), names[MGLP_IDLE],
                          g_mglp.ticks[MGLP_IDLE], divisor);
    used = mglp_append_ms(line, used, sizeof(line), " | max=",
                          g_mglp.packet_max, 0U);
    {
        written = snprintf(line + used, sizeof(line) - used,
                           " | feed svc=%lu deadline=%lu park=%lu swap=%lu empty=%lu full=%lu"
                           " retry=%lu tse=%lu tpoe=%lu dma=%lu qhi=%lu dwake=%lu"
                           " wait-ring=%lu/%lums wait-drain=%lu/%lums"
                           " wait-target=%lu/%lums wait-tex=%lu/%lums",
                           (unsigned long)g_mglp.feed_chunks[RIX_DRV_FEED_SOURCE_SVC],
                           (unsigned long)g_mglp.feed_chunks[RIX_DRV_FEED_SOURCE_SCHEDULER],
                           (unsigned long)g_mglp.feed_chunks[RIX_DRV_FEED_SOURCE_PARKED],
                           (unsigned long)g_mglp.feed_chunks[RIX_DRV_FEED_SOURCE_SWAP],
                           (unsigned long)g_mglp.feed_empty,
                           (unsigned long)g_mglp.feed_full,
                           (unsigned long)g_mglp.feed_retries,
                           (unsigned long)g_mglp.feed_tse,
                           (unsigned long)g_mglp.feed_tpoe,
                           (unsigned long)g_mglp.feed_dma_complete,
                           (unsigned long)g_mglp.queued_high_water,
                           (unsigned long)g_mglp.feed_deadline_wakeups,
                           (unsigned long)g_mglp.feed_waits[MGLP_FEED_WAIT_RING],
                           (unsigned long)(g_mglp.feed_wait_us[MGLP_FEED_WAIT_RING] /
                                           1000U / divisor),
                           (unsigned long)g_mglp.feed_waits[MGLP_FEED_WAIT_DRAIN],
                           (unsigned long)(g_mglp.feed_wait_us[MGLP_FEED_WAIT_DRAIN] /
                                           1000U / divisor),
                           (unsigned long)g_mglp.feed_waits[MGLP_FEED_WAIT_TARGET],
                           (unsigned long)(g_mglp.feed_wait_us[MGLP_FEED_WAIT_TARGET] /
                                           1000U / divisor),
                           (unsigned long)g_mglp.feed_waits[MGLP_FEED_WAIT_TEXTURE],
                           (unsigned long)(g_mglp.feed_wait_us[MGLP_FEED_WAIT_TEXTURE] /
                                           1000U / divisor));
        if (written > 0 && (size_t)written < sizeof(line) - used)
            used += (size_t)written;
        if (g_mglp.busy_valid) {
            uint32_t busy_end = minigl_platform_csr_read(0, RIX_DRV_CSR_DBG_BUSY_CYCLES);
            uint32_t busy_delta = minigl_profile_counter_delta(busy_end,
                                                                g_mglp.busy_start);
            uint64_t elapsed = g_mglp.mark - g_mglp.window_start;

            written = elapsed != 0U ?
                snprintf(line + used, sizeof(line) - used, " busy=%lu%%",
                         (unsigned long)(busy_delta / elapsed)) :
                snprintf(line + used, sizeof(line) - used, " busy=n/a");
        } else {
            written = snprintf(line + used, sizeof(line) - used, " busy=n/a");
        }
        if (written > 0 && (size_t)written < sizeof(line) - used)
            used += (size_t)written;
    }
    /* RasterIX side, per ring packet: AXI read/write bursts it issued, and
     * how often it was busy or stalled, how many reads it had in flight and
     * how much FIFO room was left whenever the next FIFO packet was ready. */
    {
        static uint32_t previous_reads;
        static uint32_t previous_writes;
        uint32_t frames = divisor;
        uint32_t samples = n[MGLP_N_HW_SAMPLES];
        uint32_t reads, writes;

        if (minigl_raster_mem_counters(&reads, &writes)) {
            uint32_t delta_reads = reads >= previous_reads ? reads - previous_reads : reads;
            uint32_t delta_writes = writes >= previous_writes ? writes - previous_writes : writes;

            previous_reads = reads;
            previous_writes = writes;
            written = snprintf(line + used, sizeof(line) - used,
                               " | hw st=%lu pgw=%lu ar=%lu aw=%lu",
                               (unsigned long)(n[MGLP_N_RIX_STATES] / frames),
                               (unsigned long)(n[MGLP_N_RIX_PAGE_WORDS] / frames),
                               (unsigned long)(delta_reads / frames),
                               (unsigned long)(delta_writes / frames));
            if (written > 0 && (size_t)written < sizeof(line) - used)
                used += (size_t)written;
        }
        if (samples != 0U) {
            uint32_t reads_x10 = (uint32_t)((uint64_t)n[MGLP_N_HW_READS] * 10U / samples);

            written = snprintf(line + used, sizeof(line) - used,
                               " sbusy=%lu%% stall=%lu%% rd=%lu.%lu vac=%lu n=%lu",
                               (unsigned long)(n[MGLP_N_HW_BUSY] * 100U / samples),
                               (unsigned long)(n[MGLP_N_HW_STALL] * 100U / samples),
                               (unsigned long)(reads_x10 / 10U),
                               (unsigned long)(reads_x10 % 10U),
                               (unsigned long)(n[MGLP_N_HW_VACANCY] / samples),
                               (unsigned long)samples);
            if (written > 0 && (size_t)written < sizeof(line) - used)
                used += (size_t)written;
        }
    }
    /* One rate-limited line: core 0 and the worker share the UART. */
    minigl_platform_log(line);
    mglp_window_reset();
    g_mglp.last_log = g_mglp.mark - print_start;
}
#endif
