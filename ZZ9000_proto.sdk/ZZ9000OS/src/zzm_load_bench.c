/*
 * Module load step instrument (-DZZM_LOAD_BENCH, firmware only).
 * See zzm_load_bench.h and docs/modules.md.
 *
 * Core 1 writes the maxima while it loads; core 0 reads and prints them.
 * Both sides use plain word accesses, so a report can show a maximum that
 * is one step stale but never a torn value.
 */

#include <stdint.h>
#include <stdio.h>
#include "zzm_load_bench.h"

#if defined(ZZM_LOAD_BENCH) || defined(ZZM_LOAD_BENCH_HOST_TEST)

#ifndef ZZM_LOAD_BENCH_HOST_TEST
#include "xtime_l.h"
#endif

#define BENCH_PRINT_INTERVAL ((uint64_t)COUNTS_PER_SECOND)

static const char *const phase_name[ZZM_LOAD_BENCH_PHASES] = {
    "open", "read", "bss", "dflush", "iflush", "init", "fail",
};

static const char *volatile module_name[ZZM_LOAD_BENCH_MODULES];
static volatile uint32_t max_ticks[ZZM_LOAD_BENCH_MODULES]
                                  [ZZM_LOAD_BENCH_PHASES];
static volatile uint32_t steps[ZZM_LOAD_BENCH_MODULES];
static volatile uint32_t dirty_mask;

static uint32_t ticks_to_us(uint32_t ticks)
{
    return (uint32_t)(((uint64_t)ticks * 1000000ULL) /
                      (uint64_t)COUNTS_PER_SECOND);
}

void zzm_load_bench_record(uint32_t module, const char *name, uint32_t phase,
                           uint64_t ticks)
{
    uint32_t capped;

    if (module >= ZZM_LOAD_BENCH_MODULES || phase >= ZZM_LOAD_BENCH_PHASES)
        return;
    capped = ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks;
    module_name[module] = name;
    steps[module]++;
    if (capped > max_ticks[module][phase]) {
        max_ticks[module][phase] = capped;
        __sync_fetch_and_or(&dirty_mask, 1U << module);
    }
}

int zzm_load_bench_format(char *buf, size_t size, uint32_t module,
                          uint32_t t_ms)
{
    if (module >= ZZM_LOAD_BENCH_MODULES || steps[module] == 0U)
        return 0;
    return snprintf(buf, size,
                    "ZZM-BENCH mod=%s %s=%luus %s=%luus %s=%luus %s=%luus "
                    "%s=%luus %s=%luus %s=%luus steps=%lu t=%lums",
                    module_name[module] ? module_name[module] : "?",
                    phase_name[0],
                    (unsigned long)ticks_to_us(max_ticks[module][0]),
                    phase_name[1],
                    (unsigned long)ticks_to_us(max_ticks[module][1]),
                    phase_name[2],
                    (unsigned long)ticks_to_us(max_ticks[module][2]),
                    phase_name[3],
                    (unsigned long)ticks_to_us(max_ticks[module][3]),
                    phase_name[4],
                    (unsigned long)ticks_to_us(max_ticks[module][4]),
                    phase_name[5],
                    (unsigned long)ticks_to_us(max_ticks[module][5]),
                    phase_name[6],
                    (unsigned long)ticks_to_us(max_ticks[module][6]),
                    (unsigned long)steps[module], (unsigned long)t_ms);
}

#ifdef ZZM_LOAD_BENCH

uint64_t zzm_load_bench_now(void)
{
    XTime now;

    XTime_GetTime(&now);
    return (uint64_t)now;
}

void zzm_load_bench_poll(void)
{
    static uint64_t last_print;
    uint64_t now = zzm_load_bench_now();
    uint32_t pending;
    uint32_t module;

    if (dirty_mask == 0U || now - last_print < BENCH_PRINT_INTERVAL)
        return;
    last_print = now;
    pending = __sync_fetch_and_and(&dirty_mask, 0U);
    for (module = 0U; module < ZZM_LOAD_BENCH_MODULES; module++) {
        char line[192];

        if ((pending & (1U << module)) == 0U ||
            zzm_load_bench_format(line, sizeof(line), module,
                                  (uint32_t)(now / ((uint64_t)COUNTS_PER_SECOND /
                                                    1000U))) == 0)
            continue;
        printf("%s\r\n", line);
    }
}

#endif /* ZZM_LOAD_BENCH */

#else /* neither flag */

typedef int zzm_load_bench_disabled;

#endif
